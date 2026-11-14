#include "metaos_motion.h"

#if CONFIG_METAOS_ENABLE

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

#include "application.h"
#include "qmi8658.h"

#define TAG "MetaMotion"

namespace metaos {

namespace {
constexpr uint32_t kSampleHz = 25;
constexpr uint32_t kSampleIntervalMs = 1000 / kSampleHz;
constexpr UBaseType_t kTaskPriority = 1;
constexpr uint32_t kTaskStackSize = 4096;
constexpr float kGravityEmaAlpha = 0.1f;      // Smooth gravity estimate
constexpr float kMovementThreshold = 200.0f;  // mg to consider "moving"
}  // namespace

MotionDetector::MotionDetector(Qmi8658* imu) : imu_(imu) {}

MotionDetector::~MotionDetector() { Stop(); }

void MotionDetector::Start(Callbacks callbacks, const Settings& settings) {
    if (imu_ == nullptr) {
        return;
    }
    if (running_) {
        return;
    }

    callbacks_ = std::move(callbacks);
    settings_ = settings;
    running_ = true;

    if (xTaskCreate(SamplingTask, "motion", kTaskStackSize, this, kTaskPriority,
                    reinterpret_cast<TaskHandle_t*>(&task_handle_)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion sampling task");
        running_ = false;
        task_handle_ = nullptr;
    } else {
        ESP_LOGI(TAG, "Motion detection started");
    }
}

void MotionDetector::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(static_cast<TaskHandle_t>(task_handle_));
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "Motion detection stopped");
}

void MotionDetector::UpdateSettings(const Settings& settings) { settings_ = settings; }

MotionDetector::State MotionDetector::GetState() const {
    State state;
    state.accel_x_mg = last_accel_x_;
    state.accel_y_mg = last_accel_y_;
    state.accel_z_mg = last_accel_z_;
    state.lying_flat = lying_flat_;
    state.moving = moving_;
    return state;
}

void MotionDetector::SamplingTask(void* arg) {
    auto* detector = static_cast<MotionDetector*>(arg);
    TickType_t last_wake = xTaskGetTickCount();

    while (detector->running_) {
        Qmi8658::AccelData accel;
        if (detector->imu_->ReadAccel(accel)) {
            detector->ProcessSample(accel.x_mg, accel.y_mg, accel.z_mg);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSampleIntervalMs));
    }

    vTaskDelete(nullptr);
}

void MotionDetector::ProcessSample(float ax, float ay, float az) {
    last_accel_x_ = ax;
    last_accel_y_ = ay;
    last_accel_z_ = az;

    // Update gravity estimate with EMA
    if (gravity_z_ == 0) {
        gravity_z_ = az;  // Initialize
    } else {
        gravity_z_ = gravity_z_ * (1.0f - kGravityEmaAlpha) + az * kGravityEmaAlpha;
    }

    // Orientation: lying flat if z-gravity is positive and strong
    lying_flat_ = (gravity_z_ > 800.0f);

    // Movement: deviation from gravity
    float deviation = std::abs(az - gravity_z_);
    moving_ = (deviation > kMovementThreshold);

    // Gesture detection
    float magnitude = std::sqrt(ax * ax + ay * ay + az * az);
    if (settings_.shake_enabled) {
        DetectShake(magnitude);
    } else {
        // Reset on the sampling-task side so a pending count cannot freeze while disabled and
        // fire a phantom skip when the gesture is re-enabled.
        shake_count_ = 0;
        shake_high_ = false;
    }
    if (settings_.flip_enabled) {
        DetectFlip(gravity_z_);
    } else {
        flip_armed_ = false;
        flipped_ = false;
        flip_triggered_ = false;
    }
}

void MotionDetector::DetectShake(float magnitude) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Register one discrete shake: above the raised threshold, re-armed near rest, and past the
    // refractory gap. Latch shake_high_ on EVERY above-threshold sample (even one the gap
    // rejects), so a single shake's rebound peak cannot straddle the gap boundary and count a
    // second time — it must first dip below kShakeRearm to re-arm.
    if (magnitude > kShakeThreshold) {
        if (!shake_high_ && (now - last_shake_time_) >= kShakeMinGap) {
            last_shake_time_ = now;
            shake_count_++;
        }
        shake_high_ = true;
    } else if (magnitude < kShakeRearm) {
        shake_high_ = false;
    }

    // After a quiet period, decide the gesture: 2 shakes -> next, 3+ -> previous, 1 -> nothing.
    if (shake_count_ > 0 && (now - last_shake_time_) >= kShakeSettleTime) {
        int n = shake_count_;
        shake_count_ = 0;
        shake_high_ = false;
        if (n == 2 && callbacks_.on_shake_next) {
            ESP_LOGI(TAG, "Shake x2 -> next track");
            Application::GetInstance().Schedule([cb = callbacks_.on_shake_next]() { cb(); });
        } else if (n >= 3 && callbacks_.on_shake_prev) {
            ESP_LOGI(TAG, "Shake x%d -> previous track", n);
            Application::GetInstance().Schedule([cb = callbacks_.on_shake_prev]() { cb(); });
        }
    }
}

void MotionDetector::DetectFlip(float gravity_z) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (gravity_z > kFlipUpThreshold) {
        // Face-up: arm the gesture and clear any face-down hold / trigger latch.
        flip_armed_ = true;
        flipped_ = false;
        flip_triggered_ = false;
    } else if (gravity_z < kFlipDownThreshold) {
        // Face-down: only counts as a flip if the device was face-up first and has not already
        // fired this session. The hold must be continuous (the band below resets it).
        if (flip_armed_ && !flip_triggered_) {
            if (!flipped_) {
                flipped_ = true;
                flip_start_ = now;
            } else if ((now - flip_start_) >= kFlipStableTime) {
                flip_triggered_ = true;
                flipped_ = false;
                ESP_LOGI(TAG, "Flip detected (face-up -> face-down held)");
                if (callbacks_.on_flip_over) {
                    Application::GetInstance().Schedule([cb = callbacks_.on_flip_over]() { cb(); });
                }
            }
        }
    } else {
        // In the hysteresis band: interrupt the hold (must be continuous), keep the armed state.
        flipped_ = false;
    }
}

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE

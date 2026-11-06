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
    }
    if (settings_.flip_enabled) {
        DetectFlip(az);
    }
}

void MotionDetector::DetectShake(float magnitude) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Look for peaks above threshold
    if (magnitude > kShakeThreshold && !shake_high_) {
        shake_high_ = true;

        // Add peak timestamp
        if (shake_peak_count_ < kShakePeaksNeeded) {
            shake_peaks_[shake_peak_count_++] = now;
        } else {
            // Shift window
            for (int i = 0; i < kShakePeaksNeeded - 1; i++) {
                shake_peaks_[i] = shake_peaks_[i + 1];
            }
            shake_peaks_[kShakePeaksNeeded - 1] = now;
        }

        // Check if we have enough peaks in the time window
        if (shake_peak_count_ == kShakePeaksNeeded) {
            uint32_t window = now - shake_peaks_[0];
            if (window <= kShakeWindow) {
                ESP_LOGI(TAG, "Shake detected!");
                shake_peak_count_ = 0;  // Reset to avoid repeated triggers
                if (callbacks_.on_shake) {
                    Application::GetInstance().Schedule([cb = callbacks_.on_shake]() { cb(); });
                }
            }
        }
    } else if (magnitude < kShakeThreshold * 0.7f) {
        shake_high_ = false;
    }

    // Age out old peaks
    if (shake_peak_count_ > 0 && (now - shake_peaks_[0]) > kShakeWindow * 2) {
        shake_peak_count_ = 0;
    }
}

void MotionDetector::DetectFlip(float z) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (z < kFlipThreshold) {
        // Device is flipped (z-axis pointing down)
        if (!flipped_) {
            flipped_ = true;
            flip_start_ = now;
        } else if ((now - flip_start_) >= kFlipStableTime) {
            // Stable flip for the required duration
            ESP_LOGI(TAG, "Flip detected!");
            flip_start_ = now + kFlipStableTime * 2;  // Cooldown to avoid repeats
            if (callbacks_.on_flip_over) {
                Application::GetInstance().Schedule([cb = callbacks_.on_flip_over]() { cb(); });
            }
        }
    } else {
        // Device returned to normal orientation
        flipped_ = false;
    }
}

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE

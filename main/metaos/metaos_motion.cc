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
        DetectShake(ax, ay, magnitude);
    } else {
        // Reset on the sampling-task side so a pending count cannot freeze while disabled and
        // fire a phantom skip when the gesture is re-enabled.
        shake_count_ = 0;
        shake_active_ = false;
    }
    if (settings_.flip_enabled) {
        DetectFlip(gravity_z_);
    } else {
        flip_armed_ = false;
        flipped_ = false;
        flip_triggered_ = false;
    }
}

void MotionDetector::DetectShake(float ax, float ay, float magnitude) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Remove the gravity/tilt DC from the horizontal axes so only the shake oscillation remains.
    ax_dc_ = ax_dc_ * (1.0f - kShakeDcAlpha) + ax * kShakeDcAlpha;
    ay_dc_ = ay_dc_ * (1.0f - kShakeDcAlpha) + ay * kShakeDcAlpha;
    float ax_ac = ax - ax_dc_;
    float ay_ac = ay - ay_dc_;

    // Vibration-reject cooldown: after a bout is discarded as sustained vibration, block new
    // bouts until the input has been quiet for a full settle period, so the vibration tail cannot
    // immediately re-arm and fire a trailing skip.
    if (magnitude >= kShakeThreshold) {
        shake_quiet_since_ = now;
    } else if (shake_reject_ && (now - shake_quiet_since_) >= kShakeSettleTime) {
        shake_reject_ = false;
    }

    // Activate a bout on motion (unless rejecting vibration). Lock the dominant axis here so
    // circular/diagonal shakes are not broken by per-sample axis chatter.
    if (magnitude > kShakeThreshold && !shake_active_ && !shake_reject_) {
        shake_active_ = true;
        shake_bout_start_ = now;
        shake_last_activity_ = now;
        shake_dom_axis_ = (std::abs(ax_ac) >= std::abs(ay_ac)) ? 0 : 1;
        float sig0 = (shake_dom_axis_ == 0) ? ax_ac : ay_ac;
        // The >2g gate already confirms real motion. If the bout opens on a positive swing, that
        // first push is a genuine cycle — seed it so counting is phase-invariant (the alternative,
        // seeding cross_high without counting, swallows the first cycle for positive-phase starts).
        if (sig0 > kShakeCrossHyst) {
            shake_cross_high_ = true;
            shake_count_ = 1;
        } else {
            shake_cross_high_ = false;
            shake_count_ = 0;
        }
    }

    if (shake_active_) {
        float sig = (shake_dom_axis_ == 0) ? ax_ac : ay_ac;
        // Schmitt trigger with a decoupled re-arm level, evaluated on EVERY sample (outside the
        // raw-magnitude gate): count on a rising crossing of +kShakeCrossHyst, re-arm once the
        // signal drops below the much lower kShakeRearmLevel. The wide hysteresis band rejects
        // vibration chatter while still counting asymmetric (weak-return) shakes and working under
        // tilt, where the raw magnitude may stay below the gate on the return stroke.
        if (sig > kShakeCrossHyst && !shake_cross_high_) {
            shake_cross_high_ = true;
            shake_count_++;
            shake_last_activity_ = now;
        } else if (sig < kShakeRearmLevel && shake_cross_high_) {
            // Edge-triggered re-arm: only the high->low transition stamps activity. Without the
            // cross_high guard, every quiet sample (sig~0 < rearm level) would refresh the timer
            // and the bout could never settle — it would always run to the vibration cap.
            shake_cross_high_ = false;
            shake_last_activity_ = now;
        }
        // Structural caps: a real gesture is brief and few cycles. Discard sustained/periodic
        // vibration and latch reject until the input is quiet (see cooldown above).
        if ((now - shake_bout_start_) > kShakeMaxBout || shake_count_ > kShakeMaxCount) {
            shake_active_ = false;
            shake_count_ = 0;
            shake_reject_ = true;
        }
    }

    // Finalize after quiet — including count==0 bouts, so a noise-locked bout always resets and
    // cannot swallow the next real shake. 2 cycles -> next, 3+ -> previous, 0/1 -> none.
    if (shake_active_ && (now - shake_last_activity_) >= kShakeSettleTime) {
        int n = shake_count_;
        shake_active_ = false;
        shake_count_ = 0;
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
        // A deliberate flip: the device was face-up first, has not fired this session, and is held
        // face-down continuously for kFlipStableTime. The filtered gravity z rides through brief
        // vibration; the hysteresis band below resets the hold only on a real orientation change.
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

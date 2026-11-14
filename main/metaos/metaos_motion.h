#ifndef METAOS_MOTION_H
#define METAOS_MOTION_H

#include <sdkconfig.h>

#if CONFIG_METAOS_ENABLE

#include <cstdint>
#include <functional>

class Qmi8658;

namespace metaos {

// Gesture detection from QMI8658 IMU sensor.
// Runs a sampling task at 25Hz and detects:
// - Shake: counts discrete shakes (raised threshold + refractory gap so one physical shake
//   counts once). After a short quiet period, 2 shakes -> next track, 3+ -> previous track.
//   A single shake does nothing, which avoids accidental skips.
// - Flip-over: the device must go from face-up to face-down and stay there steadily. A brief
//   tilt, or simply resting face-down without a flip, does not trigger.
class MotionDetector {
public:
    struct State {
        float accel_x_mg;
        float accel_y_mg;
        float accel_z_mg;
        bool lying_flat;  // z-axis pointing up (device face-up on table)
        bool moving;      // significant motion detected
    };

    struct Callbacks {
        std::function<void()> on_shake_next;  // Shaken twice (next track)
        std::function<void()> on_shake_prev;  // Shaken three times (previous track)
        std::function<void()> on_flip_over;   // Flipped face-down deliberately
    };

    struct Settings {
        bool shake_enabled = false;
        bool flip_enabled = false;
    };

    explicit MotionDetector(Qmi8658* imu);
    ~MotionDetector();

    MotionDetector(const MotionDetector&) = delete;
    MotionDetector& operator=(const MotionDetector&) = delete;

    // Start the sampling task. Callbacks run in the main task via Application::Schedule().
    void Start(Callbacks callbacks, const Settings& settings);

    // Stop the sampling task.
    void Stop();

    // Update settings (main task).
    void UpdateSettings(const Settings& settings);

    // Get current motion state (main task).
    State GetState() const;

private:
    static void SamplingTask(void* arg);
    void ProcessSample(float ax, float ay, float az);
    void DetectShake(float magnitude);
    void DetectFlip(float gravity_z);

    Qmi8658* imu_;
    Callbacks callbacks_;
    Settings settings_;
    void* task_handle_ = nullptr;
    volatile bool running_ = false;

    // State (accessed from both tasks - use atomic or simple types)
    float last_accel_x_ = 0;
    float last_accel_y_ = 0;
    float last_accel_z_ = 0;
    float gravity_z_ = 0;  // EMA filtered z for orientation
    bool lying_flat_ = false;
    bool moving_ = false;

    // Shake counting. One count per discrete shake; the refractory gap debounces the multiple
    // acceleration peaks of a single physical shake. The count is finalized after a quiet gap.
    // Thresholds are tuned to be less trigger-happy than the original 1.9g/3-peak detector and
    // may need adjustment on real hardware.
    static constexpr float kShakeThreshold = 2500.0f;  // mg (2.5g) to register a shake
    static constexpr float kShakeRearm = 1200.0f;      // mg (near rest) to re-arm
    static constexpr uint32_t kShakeMinGap = 400;      // ms refractory between counts
    static constexpr uint32_t kShakeSettleTime = 900;  // ms quiet before deciding 2 vs 3
    int shake_count_ = 0;
    uint32_t last_shake_time_ = 0;
    bool shake_high_ = false;

    // Flip detection. Requires a face-up -> face-down transition held steadily, using the
    // filtered gravity z (noise-resistant) with a hysteresis band. Re-arms only after face-up,
    // so it fires once per deliberate flip and never while just lying face-down.
    static constexpr float kFlipUpThreshold = 600.0f;    // mg, face-up
    static constexpr float kFlipDownThreshold = -600.0f; // mg, face-down
    static constexpr uint32_t kFlipStableTime = 1200;    // ms continuous face-down
    bool flip_armed_ = false;     // has been face-up (a real flip is possible)
    bool flipped_ = false;        // currently holding face-down
    bool flip_triggered_ = false; // already fired this face-down session
    uint32_t flip_start_ = 0;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
#endif  // METAOS_MOTION_H
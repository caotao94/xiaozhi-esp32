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
// - Shake: counts oscillation cycles on the dominant horizontal axis (one wrist back-and-forth =
//   one cycle). Robust to gentle/vigorous/discrete/continuous shaking. After a short quiet period,
//   2 cycles -> next track, 3+ -> previous track. A single cycle does nothing.
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
    void DetectShake(float ax, float ay, float magnitude);
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

    // Shake counting by oscillation CYCLES on the dominant horizontal axis. One adult shake (a wrist
    // back-and-forth) is one cycle. Counting cycles — not peaks or bursts — is robust to gentle,
    // vigorous, discrete and continuous shaking alike, because the intra-shake valley and the
    // inter-shake pause are physically identical and cannot be separated by a threshold gap. A
    // Schmitt trigger on the gravity-removed dominant axis counts one cycle per upward crossing; the
    // AC amplitude gate rejects speaker vibration and taps. 2 cycles -> next, 3+ -> previous, decided
    // after kShakeSettleTime of quiet; a single cycle does nothing. Tunable on hardware.
    static constexpr float kShakeThreshold = 2000.0f;  // mg (2.0g) magnitude activity gate
    static constexpr float kShakeCrossHyst = 1200.0f;  // mg AC count edge (rising crossing counts a cycle)
    static constexpr float kShakeRearmLevel = 400.0f;  // mg AC re-arm level (decoupled, weak-return safe)
    static constexpr float kShakeDcAlpha = 0.02f;      // slow EMA to remove gravity/tilt DC
    static constexpr uint32_t kShakeSettleTime = 1000; // ms quiet before deciding 2 vs 3
    static constexpr uint32_t kShakeMaxBout = 5000;    // ms max bout length (discard sustained vibration)
    static constexpr int kShakeMaxCount = 10;          // max cycles per bout (real gestures are few)
    int shake_count_ = 0;
    uint32_t shake_last_activity_ = 0;
    uint32_t shake_bout_start_ = 0;
    uint32_t shake_quiet_since_ = 0;
    bool shake_active_ = false;
    bool shake_reject_ = false;  // vibration cooldown: block new bouts until quiet
    bool shake_cross_high_ = false;
    int shake_dom_axis_ = -1;  // 0 = x, 1 = y
    float ax_dc_ = 0;
    float ay_dc_ = 0;

    // Flip detection. A deliberate flip: the device was face-up, then is turned face-down and held
    // there for kFlipStableTime. Uses the filtered gravity z (noise-resistant) with a hysteresis
    // band; re-arms only after face-up, so it fires once per flip and never while just resting
    // face-down without a flip. The continuous hold rejects brief tilts. No separate resting gate,
    // so flipping it in hand (not only set down) also works.
    static constexpr float kFlipUpThreshold = 700.0f;    // mg, face-up
    static constexpr float kFlipDownThreshold = -700.0f; // mg, face-down
    static constexpr uint32_t kFlipStableTime = 1200;    // ms continuous face-down
    bool flip_armed_ = false;     // has been face-up (a real flip is possible)
    bool flipped_ = false;        // currently holding face-down
    bool flip_triggered_ = false; // already fired this face-down session
    uint32_t flip_start_ = 0;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
#endif  // METAOS_MOTION_H
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
// - Shake: 3 peaks >1.9g within 1.2s
// - Flip-over: z-axis inverts and stays stable for >0.5s
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
        std::function<void()> on_shake;      // Device shaken (skip track)
        std::function<void()> on_flip_over;  // Device flipped face-down
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
    void DetectFlip(float z);

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

    // Shake detection
    static constexpr int kShakePeaksNeeded = 3;
    static constexpr float kShakeThreshold = 1900.0f;  // mg (1.9g)
    static constexpr uint32_t kShakeWindow = 1200;     // ms
    uint32_t shake_peaks_[kShakePeaksNeeded] = {};
    int shake_peak_count_ = 0;
    bool shake_high_ = false;

    // Flip detection
    static constexpr float kFlipThreshold = -500.0f;  // mg (z pointing down)
    static constexpr uint32_t kFlipStableTime = 500;  // ms
    bool flipped_ = false;
    uint32_t flip_start_ = 0;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
#endif  // METAOS_MOTION_H

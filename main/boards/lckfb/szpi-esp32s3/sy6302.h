#ifndef BOARDS_LCKFB_SZPI_ESP32S3_SY6302_H_
#define BOARDS_LCKFB_SZPI_ESP32S3_SY6302_H_

#include <driver/i2c_master.h>
#include <cstdint>

struct BatteryInfo;

// SY6302 battery fuel gauge (I2C 0x55) — 深圳思远半导体 / Shenzhen Think Future
// Semiconductor (TKPLUS, tkplusemi.com). Coulomb-counting gauge with an internal
// flash-programmable 32-bit CPU; its register table is NOT public (NDA-gated).
//
// REGISTER MAP — reverse-engineered on real hardware 2026-11-17 (立创·实战派
// ESP32-S3, 2000 mAh cell CSD 103450 3.7V 7.4Wh) from 39 aligned register dumps
// across charge /
// battery / recharge phases, cross-validated by current integration:
//   0x00        Gauge's own SOC estimate, u8 — MIS-SCALED for this 2000 mAh
//               cell (measured slope implies ~320 mAh); diagnostics only.
//               The displayed level is computed firmware-side instead:
//               OCV = V - I*R (R=50 mOhm, calibrated from observed load sag)
//               through a LiPo OCV table, anchored to 100% at charge
//               termination, rate-limited to +/-2% per refresh.
//   0x01-0x02   OCV estimate, u16 big-endian, mV (load-independent model value)
//   0x04-0x05   Battery current, s16 little-endian, mA. POSITIVE = charging,
//               negative = discharging. Mirrored at 0x50-0x51.
//   0x3A-0x3B   Filtered average current, s16 LE (0x8000 = charger-off sentinel)
//   0x3C-0x3D   Battery terminal voltage, u16 little-endian, mV
//
// I2C PROTOCOL (verified): write-only register-pointer byte as its OWN
// transaction (with STOP), then pure read transactions auto-increment from the
// pointer. Combined write+repeated-start (i2c_master_transmit_receive) is
// NACKed by this chip — that is why earlier BQ27220-style attempts failed.
//
// HARDWARE HAZARD: a bare read of 0x55 during boot init once hung the shared
// I2C bus (clock stretching), taking audio/IMU down into a watchdog reboot
// loop. Safety rules enforced by this driver:
//   * no I2C traffic at construction/boot;
//   * lazy attach on the first GetBatteryLevel() call, gated to >= 15 s uptime;
//   * raw non-fatal i2c_master_* calls with a 20 ms timeout (never the
//     ESP_ERROR_CHECK-based I2cDevice base class);
//   * ANY I2C error (NACK, hardware SCL timeout/clock stretch, or bus-mutex
//     timeout — IDF v6.1 reports the first two as ESP_ERR_INVALID_RESPONSE):
//     remove the device and stay permanently disabled for this boot. No
//     explicit i2c_master_bus_reset() is issued: it bypasses the bus mutex and
//     would disrupt other tasks' in-flight transactions; IDF already recovers
//     the bus synchronously under the lock on the error path and before the
//     next transaction of any device (FSM reset + 9-clock bus clear).
class Sy6302 {
public:
    static constexpr uint8_t kAddress = 0x55;

    // Register map (see class comment for evidence).
    static constexpr uint8_t kRegSoc = 0x00;
    static constexpr uint8_t kRegCurrentLo = 0x04;  // s16 LE, mA, + = charging
    static constexpr uint8_t kRegVoltageLo = 0x3C;  // u16 LE, mV

    // Construction performs NO I2C traffic; the device is attached lazily.
    explicit Sy6302(i2c_master_bus_handle_t i2c_bus);
    ~Sy6302();

    Sy6302(const Sy6302&) = delete;
    Sy6302& operator=(const Sy6302&) = delete;

    // Board::GetBatteryLevel() hook. Safe to call every second from the main
    // task: values are cached and refreshed at most every kRefreshIntervalUs.
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging);

    // Board::GetBatteryInfo() hook: cached telemetry only, NO I2C and no state
    // transitions — safe to call from UI tasks (e.g. the MetaOS panel).
    bool GetBatteryInfo(BatteryInfo& info);

private:
    enum class State {
        kPending,   // not attached yet (boot gate not passed)
        kActive,    // validated; serving cached readings
        kDisabled,  // probe/read failed — never touch 0x55 again this boot
    };

    bool Attach();                     // add device to bus (no I2C traffic)
    // rm device (deferred if bus busy) + kDisabled; deliberately NO bus reset —
    // IDF auto-recovers under the bus lock (see class comment).
    void Disable(const char* reason);
    bool WritePointer(uint8_t reg);    // pointer-write transaction (own STOP)
    bool ReadByte(uint8_t& value);     // single-byte pure read (auto-increments)
    bool Refresh();                    // read + decode + sanity-check one sample
    void DumpToSd();                   // diagnostics: aligned 256-reg dump to SD log

    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t dev_ = nullptr;
    State state_ = State::kPending;

    int64_t next_read_us_ = 0;
    int level_ = 0;
    bool have_level_ = false;
    int gauge_soc_ = 0;
    int voltage_mv_ = 0;
    int current_ma_ = 0;
    bool charging_ = false;
    bool discharging_ = false;
    int64_t last_log_us_ = 0;
    // Diagnostics mode: enabled only while /sdcard/sy6302_probe.on exists at
    // activation time; appends aligned register dumps to the SD log every 30 s
    // (used to identify SOC registers empirically). Zero cost when absent.
    bool probe_mode_ = false;
    int64_t next_dump_at_us_ = 0;
};

#endif  // BOARDS_LCKFB_SZPI_ESP32S3_SY6302_H_

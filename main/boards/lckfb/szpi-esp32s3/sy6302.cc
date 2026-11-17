#include "sy6302.h"

#include "board.h"
#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>

#define TAG "Sy6302"

namespace {

// Do not touch 0x55 during early boot; the system must be fully up first.
// (UpdateStatusBar(true) runs inside Application::Initialize — the gate makes
// those early GetBatteryLevel() calls return false without any I2C traffic.)
constexpr int64_t kBootGateUs = 15LL * 1000 * 1000;

// Per-transaction timeout. NOTE: ESP-IDF v6.1 i2c_master APIs take milliseconds
// directly (int xfer_timeout_ms), not ticks. Short on purpose: a clock-stretching
// gauge must not be able to stall the main task beyond this.
constexpr int kIoTimeoutMs = 20;

// Refresh the cached reading at most this often (the display polls every 1 s;
// the gauge itself updates on a ~1 s cycle).
constexpr int64_t kRefreshIntervalUs = 5LL * 1000 * 1000;

// Any I2C error disables the driver for this boot (fail-fast safety rule).
// NOTE: IDF v6.1 maps hardware SCL timeouts (clock stretching) AND NACKs to
// ESP_ERR_INVALID_RESPONSE; ESP_ERR_TIMEOUT only means the bus mutex could not
// be acquired. Both are treated as fatal for this chip.

// Charging/discharging hysteresis in mA: inside this band the previous state is
// kept, so end-of-charge trickle cycles (observed: current oscillating 0..+26
// mA at 4.17 V) do not flicker the UI icon.
constexpr int kChargeCurrentThresholdMa = 50;

// Sanity ranges; a reading outside these is treated as a glitch, not reported.
constexpr int kMinVoltageMv = 2500;
constexpr int kMaxVoltageMv = 5000;
constexpr int kMaxAbsCurrentMa = 5000;

// SOC is computed firmware-side from terminal voltage with IR compensation:
// the gauge's own SOC registers are mis-configured for this 2000 mAh cell
// (measured slopes imply ~320 mAh / ~3730 mAh scales; see README). OCV = V - I*R
// with R calibrated from the observed load sag (48 mV @ ~950 mA read). Because
// R was derived using the same current scale, the I*R product is invariant to
// any constant scale error in the gauge's current reading.
constexpr int kCellResistanceMOhm = 50;

// LiPo open-circuit voltage -> percent, piecewise linear (descending).
constexpr struct {
    int mv;
    int pct;
} kOcvTable[] = {
    {4200, 100}, {4150, 92}, {4100, 82}, {4050, 72}, {4000, 62}, {3950, 52},
    {3900, 42},  {3850, 32}, {3800, 22}, {3750, 14}, {3700, 8},  {3600, 4},
    {3500, 2},   {3300, 0},
};

// Rate limit so a single glitchy sample cannot jump the displayed percentage.
constexpr int kMaxLevelStep = 2;

int OcvToPercent(int ocv_mv) {
    if (ocv_mv >= kOcvTable[0].mv) {
        return 100;
    }
    constexpr int n = sizeof(kOcvTable) / sizeof(kOcvTable[0]);
    if (ocv_mv <= kOcvTable[n - 1].mv) {
        return 0;
    }
    for (int i = 0; i < n - 1; i++) {
        if (ocv_mv <= kOcvTable[i].mv && ocv_mv >= kOcvTable[i + 1].mv) {
            int span = kOcvTable[i].mv - kOcvTable[i + 1].mv;
            return kOcvTable[i + 1].pct +
                   (kOcvTable[i].pct - kOcvTable[i + 1].pct) * (ocv_mv - kOcvTable[i + 1].mv) /
                       span;
        }
    }
    return 0;
}

// Progress log cadence while active.
constexpr int64_t kLogIntervalUs = 60LL * 1000 * 1000;

// Diagnostics (file-triggered): presence of the trigger file at activation
// enables periodic aligned register dumps appended to the SD log. While USB
// MSC owns the card the FATFS mount is gone and fopen fails — that is the
// guard; battery-only runs (no USB) are exactly when dumps are wanted.
constexpr char kProbeTriggerPath[] = SD_CARD_MOUNT_POINT "/sy6302_probe.on";
constexpr char kDumpLogPath[] = SD_CARD_MOUNT_POINT "/sy6302_dump.log";
constexpr int64_t kProbeDumpIntervalUs = 30LL * 1000 * 1000;

}  // namespace

Sy6302::Sy6302(i2c_master_bus_handle_t i2c_bus) : i2c_bus_(i2c_bus) {}

Sy6302::~Sy6302() {
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

bool Sy6302::Attach() {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAddress,
        // The chip supports 400 kHz; stay at the bus-proven 100 kHz for margin.
        .scl_speed_hz = 100 * 1000,
        .scl_wait_us = 0,  // driver default
        .flags =
            {
                .disable_ack_check = 0,
            },
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_, &cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device 0x%02X to bus: %s", kAddress, esp_err_to_name(err));
        dev_ = nullptr;
        return false;
    }
    return true;
}

void Sy6302::Disable(const char* reason) {
    ESP_LOGW(TAG, "SY6302 permanently disabled for this boot: %s", reason);
    if (dev_ != nullptr) {
        esp_err_t err = i2c_master_bus_rm_device(dev_);
        if (err == ESP_OK) {
            dev_ = nullptr;
        } else {
            // Bus busy with another task's transaction (ESP_ERR_INVALID_STATE):
            // keep the handle (destructor retries); kDisabled guarantees we
            // never transact on it again. Leaving it attached is harmless.
            ESP_LOGW(TAG, "rm_device deferred (%s); handle kept for cleanup",
                     esp_err_to_name(err));
        }
    }
    // Deliberately NO i2c_master_bus_reset() here: it does not take the bus
    // mutex and would disrupt in-flight codec/touch transactions from other
    // tasks (their ESP_ERROR_CHECK would abort). IDF v6.1 already recovers
    // synchronously under the bus lock: the failing transaction's error path
    // resets the FSM, and the next transaction of ANY device performs an FSM
    // reset + 9-clock bus clear when status==TIMEOUT or the bus is stuck
    // (s_i2c_transaction_start). Recovery is therefore automatic and safe.
    state_ = State::kDisabled;
}

bool Sy6302::WritePointer(uint8_t reg) {
    // Own transaction with STOP — the gauge NACKs combined write+repeated-start.
    esp_err_t err = i2c_master_transmit(dev_, &reg, 1, kIoTimeoutMs);
    if (err != ESP_OK) {
        // Covers NACK, hardware SCL timeout (clock stretch) and bus-mutex
        // timeout alike — all fatal for this chip per the fail-fast rule.
        char reason[64];
        snprintf(reason, sizeof(reason), "pointer 0x%02X write: %s", reg, esp_err_to_name(err));
        Disable(reason);
        return false;
    }
    return true;
}

bool Sy6302::ReadByte(uint8_t& value) {
    esp_err_t err = i2c_master_receive(dev_, &value, 1, kIoTimeoutMs);
    if (err != ESP_OK) {
        char reason[64];
        snprintf(reason, sizeof(reason), "read: %s", esp_err_to_name(err));
        Disable(reason);
        return false;
    }
    return true;
}

bool Sy6302::Refresh() {
    // Registers come from single-byte auto-incrementing reads: set the pointer,
    // then walk forward. Total: 2 pointer writes + 8 byte reads (~2 ms).
    uint8_t soc = 0, skip = 0, i_lo = 0, i_hi = 0, v_lo = 0, v_hi = 0;

    if (!WritePointer(kRegSoc) || !ReadByte(soc)) {
        return false;
    }
    // Walk 0x01..0x03 (OCV high/low + spare) to reach the current at 0x04.
    for (int i = 0; i < 3; i++) {
        if (!ReadByte(skip)) {
            return false;
        }
    }
    if (!ReadByte(i_lo) || !ReadByte(i_hi)) {
        return false;
    }
    if (!WritePointer(kRegVoltageLo) || !ReadByte(v_lo) || !ReadByte(v_hi)) {
        return false;
    }

    int current_ma = (int16_t)((uint16_t)i_lo | ((uint16_t)i_hi << 8));
    int voltage_mv = (int)((uint16_t)v_lo | ((uint16_t)v_hi << 8));

    // Sanity: plausible values only. An out-of-range sample is skipped (the
    // gauge may be mid-update) and the previous cached value is kept; I2C
    // errors above already disabled the driver immediately.
    // Only V/I sanity gates the sample: reg00 (gauge SOC) is known mis-scaled
    // for this cell and is diagnostics-only — it must never veto healthy
    // readings nor disable the driver.
    if (voltage_mv < kMinVoltageMv || voltage_mv > kMaxVoltageMv ||
        current_ma < -kMaxAbsCurrentMa || current_ma > kMaxAbsCurrentMa) {
        ESP_LOGW(TAG, "Implausible sample skipped: soc=%d v=%d mV i=%d mA", soc, voltage_mv,
                 current_ma);
        return false;
    }

    voltage_mv_ = voltage_mv;
    current_ma_ = current_ma;
    gauge_soc_ = soc;  // gauge's own (mis-scaled) estimate, kept for diagnostics

    // Firmware-side SOC: IR-compensated OCV + LiPo table (see constants above).
    int ocv_mv = voltage_mv - current_ma * kCellResistanceMOhm / 1000;
    if (ocv_mv < 3300) {
        ocv_mv = 3300;
    } else if (ocv_mv > 4250) {
        ocv_mv = 4250;
    }
    int target = OcvToPercent(ocv_mv);
    // Charge-termination anchor: near-full voltage with taper/idle current.
    if (current_ma <= 60 && current_ma > -50 && voltage_mv >= 4150) {
        target = 100;
    }
    if (level_ == 0 && !have_level_) {
        level_ = target;  // first sample: adopt directly
        have_level_ = true;
    } else {
        int step = target - level_;
        if (step > kMaxLevelStep) {
            step = kMaxLevelStep;
        } else if (step < -kMaxLevelStep) {
            step = -kMaxLevelStep;
        }
        level_ += step;
    }
    // Hysteresis: inside the band keep the previous state (trickle cycles),
    // except at near-full voltage where small currents mean idle-on-USB, not
    // a latched discharge from the boot-time system-draw sample.
    if (current_ma > kChargeCurrentThresholdMa) {
        charging_ = true;
        discharging_ = false;
    } else if (current_ma < -kChargeCurrentThresholdMa) {
        charging_ = false;
        discharging_ = true;
    } else if (voltage_mv >= 4150) {
        charging_ = false;
        discharging_ = false;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us_ >= kLogIntervalUs) {
        last_log_us_ = now_us;
        ESP_LOGI(TAG, "Battery: %d%%, %d mV, %+d mA (%s, gauge soc=%d)", level_, voltage_mv_,
                 current_ma_, charging_ ? "charging" : (discharging_ ? "discharging" : "idle"),
                 gauge_soc_);
    }
    return true;
}

void Sy6302::DumpToSd() {
    // Serial always (probe mode only); SD append best-effort (fails while USB
    // MSC owns the card — battery-only runs are when the SD copy matters).
    FILE* log = fopen(kDumpLogPath, "a");
    char line[96];
    snprintf(line, sizeof(line), "=== dump t=%lld s ===",
             (long long)(esp_timer_get_time() / 1000000));
    ESP_LOGI(TAG, "%s", line);
    if (log != nullptr) {
        fprintf(log, "%s\n", line);
    }
    if (!WritePointer(kRegSoc)) {
        if (log != nullptr) fclose(log);
        return;
    }
    for (int base = 0; base < 256; base += 16) {
        uint8_t row[16];
        for (int i = 0; i < 16; i++) {
            if (!ReadByte(row[i])) {
                if (log != nullptr) fclose(log);
                return;  // helpers already disabled the driver on I2C error
            }
        }
        snprintf(line, sizeof(line),
                 "%02X: %02X %02X %02X %02X %02X %02X %02X %02X "
                 "%02X %02X %02X %02X %02X %02X %02X %02X",
                 base, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8],
                 row[9], row[10], row[11], row[12], row[13], row[14], row[15]);
        ESP_LOGI(TAG, "%s", line);
        if (log != nullptr) {
            fprintf(log, "%s\n", line);
        }
    }
    if (log != nullptr) {
        fclose(log);
    }
}

bool Sy6302::GetBatteryInfo(BatteryInfo& info) {
    if (state_ != State::kActive) {
        return false;
    }
    info.level = level_;
    info.voltage_mv = voltage_mv_;
    info.current_ma = current_ma_;
    info.charging = charging_;
    info.discharging = discharging_;
    return true;
}

bool Sy6302::GetBatteryLevel(int& level, bool& charging, bool& discharging) {
    switch (state_) {
        case State::kDisabled:
            return false;

        case State::kPending: {
            // Boot gate: no I2C traffic to 0x55 until the system is fully up.
            if (esp_timer_get_time() < kBootGateUs) {
                return false;
            }
            if (!Attach()) {
                // add_device performs no bus traffic; a bus reset is not needed.
                state_ = State::kDisabled;
                return false;
            }
            // Validate with real samples before trusting the chip. A single
            // implausible sample may just be a mid-update read; retry a few
            // times before giving up (I2C timeouts disable immediately inside
            // Refresh's helpers).
            bool validated = false;
            for (int attempt = 0; attempt < 3 && state_ != State::kDisabled; attempt++) {
                if (Refresh()) {
                    validated = true;
                    break;
                }
            }
            if (!validated) {
                if (state_ != State::kDisabled) {
                    Disable("validation samples failed");
                }
                return false;
            }
            state_ = State::kActive;
            next_read_us_ = esp_timer_get_time() + kRefreshIntervalUs;
            // Diagnostics trigger: only sampled once at activation.
            FILE* trigger = fopen(kProbeTriggerPath, "r");
            if (trigger != nullptr) {
                fclose(trigger);
                probe_mode_ = true;
                ESP_LOGI(TAG, "Probe mode ON (trigger file present): SD dumps every 30 s");
                DumpToSd();
                if (state_ == State::kDisabled) {
                    return false;  // dump hit an I2C error and disabled us
                }
                next_dump_at_us_ = esp_timer_get_time() + kProbeDumpIntervalUs;
            }
            ESP_LOGI(TAG, "SY6302 active: %d%%, %d mV, %+d mA", level_, voltage_mv_, current_ma_);
            level = level_;
            charging = charging_;
            discharging = discharging_;
            return true;
        }

        case State::kActive: {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= next_read_us_) {
                next_read_us_ = now_us + kRefreshIntervalUs;
                Refresh();  // on I2C failure this disables and falls through
                if (state_ == State::kDisabled) {
                    return false;
                }
            }
            if (probe_mode_ && now_us >= next_dump_at_us_) {
                next_dump_at_us_ = now_us + kProbeDumpIntervalUs;
                DumpToSd();
                if (state_ == State::kDisabled) {
                    return false;
                }
            }
            level = level_;
            charging = charging_;
            discharging = discharging_;
            return true;
        }
    }
    return false;
}

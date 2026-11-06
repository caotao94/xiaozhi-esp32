#include "qmi8658.h"

#if CONFIG_METAOS_ENABLE

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <new>

#define TAG "QMI8658"

// Register addresses
#define QMI8658_WHO_AM_I 0x00
#define QMI8658_CTRL1 0x02
#define QMI8658_CTRL2 0x03
#define QMI8658_CTRL3 0x04
#define QMI8658_CTRL4 0x05
#define QMI8658_CTRL5 0x06
#define QMI8658_CTRL7 0x08
#define QMI8658_AX_L 0x35
#define QMI8658_AX_H 0x36
#define QMI8658_AY_L 0x37
#define QMI8658_AY_H 0x38
#define QMI8658_AZ_L 0x39
#define QMI8658_AZ_H 0x3A
#define QMI8658_GX_L 0x3B
#define QMI8658_GX_H 0x3C
#define QMI8658_GY_L 0x3D
#define QMI8658_GY_H 0x3E
#define QMI8658_GZ_L 0x3F
#define QMI8658_GZ_H 0x40

#define QMI8658_EXPECTED_WHO_AM_I 0x05

// Accelerometer ranges (±g)
#define QMI8658_ACC_RANGE_2G 0
#define QMI8658_ACC_RANGE_4G 1
#define QMI8658_ACC_RANGE_8G 2
#define QMI8658_ACC_RANGE_16G 3

// Gyroscope ranges (±dps)
#define QMI8658_GYR_RANGE_16DPS 0
#define QMI8658_GYR_RANGE_32DPS 1
#define QMI8658_GYR_RANGE_64DPS 2
#define QMI8658_GYR_RANGE_128DPS 3
#define QMI8658_GYR_RANGE_256DPS 4
#define QMI8658_GYR_RANGE_512DPS 5
#define QMI8658_GYR_RANGE_1024DPS 6
#define QMI8658_GYR_RANGE_2048DPS 7

Qmi8658* Qmi8658::Probe(i2c_master_bus_handle_t i2c_bus) {
    // Try standard addresses
    const uint8_t addresses[] = {0x6A, 0x6B};
    for (uint8_t addr : addresses) {
        auto* device = new (std::nothrow) Qmi8658(i2c_bus, addr);
        if (device != nullptr && device->Initialize()) {
            uint8_t who_am_i = device->GetDeviceId();
            if (who_am_i == QMI8658_EXPECTED_WHO_AM_I) {
                ESP_LOGI(TAG, "Found QMI8658 at address 0x%02X", addr);
                return device;
            }
            ESP_LOGW(TAG, "Device at 0x%02X has wrong WHO_AM_I: 0x%02X", addr, who_am_i);
        }
        delete device;
    }
    ESP_LOGW(TAG, "QMI8658 not found");
    return nullptr;
}

Qmi8658::Qmi8658(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : i2c_bus_(i2c_bus), dev_handle_(nullptr), address_(address), accel_scale_(0), gyro_scale_(0) {}

Qmi8658::~Qmi8658() {
    if (dev_handle_ != nullptr) {
        i2c_master_bus_rm_device(dev_handle_);
    }
}

bool Qmi8658::Initialize() {
    // Create I2C device handle
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address_;
    dev_cfg.scl_speed_hz = 400000;  // 400 kHz

    if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device at 0x%02X", address_);
        return false;
    }

    // Software reset
    if (!WriteReg(QMI8658_CTRL1, 0x80)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configure accelerometer: ±4g range, 25Hz ODR
    // CTRL2: [7:4]=ODR, [3:1]=range, [0]=enable
    // ODR: 0x5 = 25Hz, Range: 1 = ±4g
    if (!WriteReg(QMI8658_CTRL2, (0x5 << 4) | (QMI8658_ACC_RANGE_4G << 1) | 0x01)) {
        return false;
    }
    accel_scale_ = 4000.0f / 32768.0f;  // ±4g range: 4000mg / 32768 counts

    // Configure gyroscope: ±512dps range, 25Hz ODR
    // CTRL3: [7:4]=ODR, [3:1]=range, [0]=enable
    if (!WriteReg(QMI8658_CTRL3, (0x5 << 4) | (QMI8658_GYR_RANGE_512DPS << 1) | 0x01)) {
        return false;
    }
    gyro_scale_ = 512.0f / 32768.0f;  // ±512dps range

    // Enable sensors in CTRL7
    if (!WriteReg(QMI8658_CTRL7, 0x03)) {  // Enable accel and gyro
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

uint8_t Qmi8658::GetDeviceId() {
    uint8_t id = 0;
    ReadReg(QMI8658_WHO_AM_I, id);
    return id;
}

bool Qmi8658::ReadAccel(AccelData& data) {
    uint8_t raw[6];
    if (!ReadRegs(QMI8658_AX_L, raw, 6)) {
        return false;
    }

    data.x = static_cast<int16_t>((raw[1] << 8) | raw[0]);
    data.y = static_cast<int16_t>((raw[3] << 8) | raw[2]);
    data.z = static_cast<int16_t>((raw[5] << 8) | raw[4]);

    data.x_mg = data.x * accel_scale_;
    data.y_mg = data.y * accel_scale_;
    data.z_mg = data.z * accel_scale_;

    return true;
}

bool Qmi8658::ReadGyro(GyroData& data) {
    uint8_t raw[6];
    if (!ReadRegs(QMI8658_GX_L, raw, 6)) {
        return false;
    }

    data.x = static_cast<int16_t>((raw[1] << 8) | raw[0]);
    data.y = static_cast<int16_t>((raw[3] << 8) | raw[2]);
    data.z = static_cast<int16_t>((raw[5] << 8) | raw[4]);

    data.x_dps = data.x * gyro_scale_;
    data.y_dps = data.y * gyro_scale_;
    data.z_dps = data.z * gyro_scale_;

    return true;
}

bool Qmi8658::ReadBoth(AccelData& accel, GyroData& gyro) {
    return ReadAccel(accel) && ReadGyro(gyro);
}

bool Qmi8658::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(dev_handle_, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Write reg 0x%02X failed: %d", reg, ret);
        return false;
    }
    return true;
}

bool Qmi8658::ReadReg(uint8_t reg, uint8_t& value) {
    esp_err_t ret =
        i2c_master_transmit_receive(dev_handle_, &reg, 1, &value, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read reg 0x%02X failed: %d", reg, ret);
        return false;
    }
    return true;
}

bool Qmi8658::ReadRegs(uint8_t reg, uint8_t* buffer, size_t len) {
    esp_err_t ret =
        i2c_master_transmit_receive(dev_handle_, &reg, 1, buffer, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read regs from 0x%02X failed: %d", reg, ret);
        return false;
    }
    return true;
}

#endif  // CONFIG_METAOS_ENABLE

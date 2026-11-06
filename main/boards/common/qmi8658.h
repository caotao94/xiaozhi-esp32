#ifndef QMI8658_H
#define QMI8658_H

#include <sdkconfig.h>

#if CONFIG_METAOS_ENABLE

#include <driver/i2c_master.h>
#include <cstdint>

// QMI8658 6-axis IMU driver (accelerometer + gyroscope).
// Supports I2C communication at addresses 0x6A or 0x6B.
class Qmi8658 {
public:
    struct AccelData {
        int16_t x;  // raw counts
        int16_t y;
        int16_t z;
        float x_mg;  // milligravity
        float y_mg;
        float z_mg;
    };

    struct GyroData {
        int16_t x;  // raw counts
        int16_t y;
        int16_t z;
        float x_dps;  // degrees per second
        float y_dps;
        float z_dps;
    };

    // Probe for the device at standard addresses. Returns nullptr if not found.
    static Qmi8658* Probe(i2c_master_bus_handle_t i2c_bus);

    ~Qmi8658();

    // Read accelerometer data. Returns false on I2C error.
    bool ReadAccel(AccelData& data);

    // Read gyroscope data. Returns false on I2C error.
    bool ReadGyro(GyroData& data);

    // Read both sensors. Returns false on I2C error.
    bool ReadBoth(AccelData& accel, GyroData& gyro);

    // Get WHO_AM_I register (should be 0x05).
    uint8_t GetDeviceId();

private:
    Qmi8658(i2c_master_bus_handle_t i2c_bus, uint8_t address);
    bool Initialize();
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadReg(uint8_t reg, uint8_t& value);
    bool ReadRegs(uint8_t reg, uint8_t* buffer, size_t len);

    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t dev_handle_;
    uint8_t address_;
    float accel_scale_;  // mg per LSB
    float gyro_scale_;   // dps per LSB
};

#endif  // CONFIG_METAOS_ENABLE
#endif  // QMI8658_H

#ifndef QMI8658_SENSOR_H
#define QMI8658_SENSOR_H

#include "i2c_device.h"
#include <stdint.h>

class Qmi8658Sensor : public I2cDevice {
public:
    enum AccScale {
        ACC_RANGE_2G = 0x0,
        ACC_RANGE_4G,
        ACC_RANGE_8G,
        ACC_RANGE_16G
    };
    enum GyroScale {
        GYR_RANGE_16DPS = 0x0,
        GYR_RANGE_32DPS,
        GYR_RANGE_64DPS,
        GYR_RANGE_128DPS,
        GYR_RANGE_256DPS,
        GYR_RANGE_512DPS,
        GYR_RANGE_1024DPS
    };
    enum AccOdr {
        acc_odr_norm_8000 = 0x0,
        acc_odr_norm_4000,
        acc_odr_norm_2000,
        acc_odr_norm_1000,
        acc_odr_norm_500,
        acc_odr_norm_250,
        acc_odr_norm_120,
        acc_odr_norm_60,
        acc_odr_norm_30,
        acc_odr_lp_128 = 0xC,
        acc_odr_lp_21,
        acc_odr_lp_11,
        acc_odr_lp_3,
    };
    enum GyroOdr {
        gyro_odr_norm_8000 = 0x0,
        gyro_odr_norm_4000,
        gyro_odr_norm_2000,
        gyro_odr_norm_1000,
        gyro_odr_norm_500,
        gyro_odr_norm_250,
        gyro_odr_norm_120,
        gyro_odr_norm_60,
        gyro_odr_norm_30
    };
    enum LpfMode {
        LPF_MODE_0 = 0x0,     //2.66% of ODR
        LPF_MODE_1 = 0x2,     //3.63% of ODR
        LPF_MODE_2 = 0x4,     //5.39% of ODR
        LPF_MODE_3 = 0x6      //13.37% of ODR
    };

    struct Vec3f { float x, y, z; };

    // addr default: 0x6B (SD0/SA0 low). Use 0x6A if pin strapped high.
    Qmi8658Sensor(i2c_master_bus_handle_t i2c_bus, uint8_t addr = 0x6B);

    bool Init(AccScale acc_scale = ACC_RANGE_4G,
              GyroScale gyro_scale = GYR_RANGE_64DPS,
              AccOdr acc_odr = acc_odr_norm_8000,
              GyroOdr gyro_odr = gyro_odr_norm_8000,
              LpfMode acc_lpf = LPF_MODE_0,
              LpfMode gyro_lpf = LPF_MODE_3);

    // Read raw, scaled values (g for accel, dps for gyro)
    Vec3f ReadAccel();
    Vec3f ReadGyro();

    // Non-throwing try-reads: return false on I2C error (no abort)
    bool TryReadAccel(Vec3f* out);
    bool TryReadGyro(Vec3f* out);

private:
    uint8_t addr_;
    float accel_scale_ = 0.0f;
    float gyro_scale_ = 0.0f;

    // Registers
    static constexpr uint8_t REG_WHO_AM_I = 0x00;
    static constexpr uint8_t REG_REV_ID   = 0x01;
    static constexpr uint8_t REG_CTRL1    = 0x02;
    static constexpr uint8_t REG_CTRL2    = 0x03;
    static constexpr uint8_t REG_CTRL3    = 0x04;
    static constexpr uint8_t REG_CTRL5    = 0x06;
    static constexpr uint8_t REG_CTRL6    = 0x07;
    static constexpr uint8_t REG_CTRL7    = 0x08;

    static constexpr uint8_t REG_AX_L     = 0x35;
    static constexpr uint8_t REG_GX_L     = 0x3B;

    // Bit fields
    static constexpr uint8_t ASCALE_MASK = 0x70;
    static constexpr uint8_t GSCALE_MASK = 0x70;
    static constexpr uint8_t AODR_MASK   = 0x0F;
    static constexpr uint8_t GODR_MASK   = 0x0F;
    static constexpr uint8_t ASCALE_OFF  = 4;
    static constexpr uint8_t GSCALE_OFF  = 4;
    static constexpr uint8_t ALPF_MASK   = 0x06;
    static constexpr uint8_t GLPF_MASK   = 0x60;
    static constexpr uint8_t ALPF_OFF    = 1;
    static constexpr uint8_t GLPF_OFF    = 5;

    void SetStateRunning();
    void SetAccScale(AccScale scale);
    void SetGyroScale(GyroScale scale);
    void SetAccOdr(AccOdr odr);
    void SetGyroOdr(GyroOdr odr);
    void SetAccLpf(LpfMode lpf);
    void SetGyroLpf(LpfMode lpf);
};

#endif // QMI8658_SENSOR_H


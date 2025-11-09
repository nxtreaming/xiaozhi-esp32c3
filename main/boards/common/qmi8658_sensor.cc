#include "qmi8658_sensor.h"

#include <esp_log.h>

static const char* TAG_QMI = "QMI8658";
#include <driver/i2c_master.h>


Qmi8658Sensor::Qmi8658Sensor(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr), addr_(addr) {}

bool Qmi8658Sensor::Init(AccScale acc_scale, GyroScale gyro_scale, AccOdr acc_odr, GyroOdr gyro_odr, LpfMode acc_lpf, LpfMode gyro_lpf) {
    uint8_t rev = ReadReg(REG_REV_ID);
    ESP_LOGI(TAG_QMI, "QMI8658 Revision ID: 0x%02X (addr 0x%02X)", rev, addr_);

    SetStateRunning();

    SetAccScale(acc_scale);
    switch (acc_scale) {
        case ACC_RANGE_2G:  accel_scale_ = 2.0f  / 32768.0f; break;
        case ACC_RANGE_4G:  accel_scale_ = 4.0f  / 32768.0f; break;
        case ACC_RANGE_8G:  accel_scale_ = 8.0f  / 32768.0f; break;
        case ACC_RANGE_16G: accel_scale_ = 16.0f / 32768.0f; break;
    }
    SetAccOdr(acc_odr);
    SetAccLpf(acc_lpf);

    SetGyroScale(gyro_scale);
    switch (gyro_scale) {
        case GYR_RANGE_16DPS:   gyro_scale_ = 16.0f   / 32768.0f; break;
        case GYR_RANGE_32DPS:   gyro_scale_ = 32.0f   / 32768.0f; break;
        case GYR_RANGE_64DPS:   gyro_scale_ = 64.0f   / 32768.0f; break;
        case GYR_RANGE_128DPS:  gyro_scale_ = 128.0f  / 32768.0f; break;
        case GYR_RANGE_256DPS:  gyro_scale_ = 256.0f  / 32768.0f; break;
        case GYR_RANGE_512DPS:  gyro_scale_ = 512.0f  / 32768.0f; break;
        case GYR_RANGE_1024DPS: gyro_scale_ = 1024.0f / 32768.0f; break;
    }
    SetGyroOdr(gyro_odr);
    SetGyroLpf(gyro_lpf);

    return true;
}

void Qmi8658Sensor::SetStateRunning() {
    // CTRL1: enable 2MHz oscillator (clear bit0) and auto address increment (set bit6)
    uint8_t ctrl1 = ReadReg(REG_CTRL1);
    ctrl1 &= 0xFE; // clear bit0
    ctrl1 |= 0x40; // set bit6
    WriteReg(REG_CTRL1, ctrl1);

    // CTRL7: high speed internal clock, acc & gyro full mode, disable syncSample
    WriteReg(REG_CTRL7, 0x43);

    // CTRL6: disable AttitudeEngine Motion On Demand
    WriteReg(REG_CTRL6, 0x00);
}

void Qmi8658Sensor::SetAccScale(AccScale scale) {
    uint8_t v = ReadReg(REG_CTRL2);
    v &= ~ASCALE_MASK;
    v |= (static_cast<uint8_t>(scale) << ASCALE_OFF);
    WriteReg(REG_CTRL2, v);
}

void Qmi8658Sensor::SetGyroScale(GyroScale scale) {
    uint8_t v = ReadReg(REG_CTRL3);
    v &= ~GSCALE_MASK;
    v |= (static_cast<uint8_t>(scale) << GSCALE_OFF);
    WriteReg(REG_CTRL3, v);
}

void Qmi8658Sensor::SetAccOdr(AccOdr odr) {
    uint8_t v = ReadReg(REG_CTRL2);
    v &= ~AODR_MASK;
    v |= static_cast<uint8_t>(odr);
    WriteReg(REG_CTRL2, v);
}

void Qmi8658Sensor::SetGyroOdr(GyroOdr odr) {
    uint8_t v = ReadReg(REG_CTRL3);
    v &= ~GODR_MASK;
    v |= static_cast<uint8_t>(odr);
    WriteReg(REG_CTRL3, v);
}

void Qmi8658Sensor::SetAccLpf(LpfMode lpf) {
    uint8_t v = ReadReg(REG_CTRL5);
    v &= ~ALPF_MASK;
    v |= (static_cast<uint8_t>(lpf) << ALPF_OFF);
    v |= 0x01; // turn on acc LPF
    WriteReg(REG_CTRL5, v);
}

void Qmi8658Sensor::SetGyroLpf(LpfMode lpf) {
    uint8_t v = ReadReg(REG_CTRL5);
    v &= ~GLPF_MASK;
    v |= (static_cast<uint8_t>(lpf) << GLPF_OFF);
    v |= 0x10; // turn on gyro LPF
    WriteReg(REG_CTRL5, v);
}

Qmi8658Sensor::Vec3f Qmi8658Sensor::ReadAccel() {
    uint8_t buf[6];
    ReadRegs(REG_AX_L, buf, 6);
    int16_t x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t z = (int16_t)((buf[5] << 8) | buf[4]);
    return Vec3f{ x * accel_scale_, y * accel_scale_, z * accel_scale_ };
}

Qmi8658Sensor::Vec3f Qmi8658Sensor::ReadGyro() {
    uint8_t buf[6];
    ReadRegs(REG_GX_L, buf, 6);
    int16_t x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t z = (int16_t)((buf[5] << 8) | buf[4]);
    return Vec3f{ x * gyro_scale_, y * gyro_scale_, z * gyro_scale_ };
}

bool Qmi8658Sensor::TryReadAccel(Vec3f* out) {
    if (!out) return false;
    uint8_t reg = REG_AX_L;
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buf, 6, 200 /*ms*/);
    if (err != ESP_OK) {
        return false;
    }
    int16_t x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t z = (int16_t)((buf[5] << 8) | buf[4]);
    *out = Vec3f{ x * accel_scale_, y * accel_scale_, z * accel_scale_ };
    return true;
}

bool Qmi8658Sensor::TryReadGyro(Vec3f* out) {
    if (!out) return false;
    uint8_t reg = REG_GX_L;
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buf, 6, 200 /*ms*/);
    if (err != ESP_OK) {
        static int s_fail;
        if (((++s_fail) & 0x0F) == 1) { // rate-limit logs
            ESP_LOGW(TAG_QMI, "TryReadGyro I2C error: %s", esp_err_to_name(err));
        }
        return false;
    }
    int16_t x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t z = (int16_t)((buf[5] << 8) | buf[4]);
    *out = Vec3f{ x * gyro_scale_, y * gyro_scale_, z * gyro_scale_ };
    return true;
}


#include <MPU9250.h>
#define NODE_LCD 1
#include "node_common.h"

MPU9250 mpu;
bool imu_ok = false;

void sensor_setup() {
  Wire.begin(NODE_I2C_SDA, NODE_I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(1000);
  lcd.setBusClocks(100000, 400000);
  lcd_init();
  lcd_message("Telemetry node", "IMU init...");

  DBG.println("\nInitializing MPU9250 IMU...");
  imu_ok = mpu.setup(0x68);
  if (!imu_ok) {
    DBG.println("[ERROR] IMU not found, telemetry will report zero roll/pitch/yaw.");
    lcd_message("IMU not found", "check wiring");
    return;
  }
  DBG.println("[OK] IMU connected.");

  DBG.println("Calibrating accelerometer and gyroscope, keep the sensor still...");
  lcd_message("Calibrating IMU", "keep it still");
  delay(2000);
  mpu.calibrateAccelGyro();
  DBG.println("Calibration complete.");
  lcd_message("IMU ready", "");
}

void sensor_update() {
  if (imu_ok) mpu.update();
}

bool sensor_payload(char *buf, size_t n) {
  snprintf(buf, n, "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
           mpu.getRoll(), mpu.getPitch(), mpu.getYaw());
  return true;
}

bool sensor_display(char *line, size_t n) {
  if (!imu_ok) {
    snprintf(line, n, "IMU not found");
    return true;
  }
  snprintf(line, n, "R%ld P%ld Y%ld", lroundf(mpu.getRoll()), lroundf(mpu.getPitch()), lroundf(mpu.getYaw()));
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }

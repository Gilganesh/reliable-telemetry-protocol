#include <MPU9250.h>
#include "node_common.h"

MPU9250 mpu;
bool imu_ok = false;

void sensor_setup() {
  Wire.begin(21, 22);
  Wire.setClock(400000);
  Wire.setTimeOut(1000);

  DBG.println("\nInitializing MPU9250 IMU...");
  imu_ok = mpu.setup(0x68);
  if (!imu_ok) {
    DBG.println("[ERROR] IMU not found, telemetry will report zero roll/pitch/yaw.");
    return;
  }
  DBG.println("[OK] IMU connected.");

  DBG.println("Calibrating accelerometer and gyroscope, keep the sensor still...");
  delay(2000);
  mpu.calibrateAccelGyro();
  DBG.println("Calibration complete.");
}

void sensor_update() {
  if (imu_ok) mpu.update();
}

bool sensor_payload(char *buf, size_t n) {
  snprintf(buf, n, "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
           mpu.getRoll(), mpu.getPitch(), mpu.getYaw());
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }

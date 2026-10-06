#include <MPU9250.h>

MPU9250 mpu;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  delay(1000);
  Serial.println("Initializing MPU9250...");

  if (!mpu.setup(0x68)) {
    Serial.println("MPU9250 not found, check wiring and I2C address.");
    while (1) delay(10);
  }
  Serial.println("MPU9250 found");

  Serial.println("Calibrating accelerometer and gyroscope...");
  Serial.println("Keep the sensor still.");
  delay(2000);

  mpu.calibrateAccelGyro();
  Serial.println("Calibration complete, streaming data.\n");
}

void loop() {
  if (mpu.update()) {
    Serial.print("Acc X: ");
    Serial.print(mpu.getAccX(), 3);
    Serial.print("  Y: ");
    Serial.print(mpu.getAccY(), 3);
    Serial.print("  Z: ");
    Serial.print(mpu.getAccZ(), 3);

    Serial.print("  |  Gyro X: ");
    Serial.print(mpu.getGyroX(), 2);
    Serial.print("  Y: ");
    Serial.print(mpu.getGyroY(), 2);
    Serial.print("  Z: ");
    Serial.println(mpu.getGyroZ(), 2);
  }

  delay(100);
}

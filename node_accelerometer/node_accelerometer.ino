/*
 * node_accelerometer.ino -- вузол з IMU MPU9250 (акселерометр + гіроскоп).
 *
 * Уся робота зі зв'язком (канали UART>TCP>UDP, буфер, ALARM з ACK/retry,
 * автоналаштування від шлюзу) -- у спільному node_common.h. Тут лише датчик:
 * ініціалізація, калібрування й формування payload {"roll","pitch","yaw"}.
 *
 * ПОТРІБНА БІБЛІОТЕКА (Arduino IDE -> Tools -> Manage Libraries):
 *   "MPU9250" автора hideakitai (є схожі назви для інших датчиків -- шукай саме його).
 *
 * Підключення (класична ESP32, I2C): SDA = GPIO21, SCL = GPIO22, адреса 0x68.
 *
 * Поруч мають лежати копії node_common.h, packet_queue.h, protocol.*, reliability.*
 * (node_common/sync.sh розкладає їх автоматично).
 */
#include <MPU9250.h>
#include "node_common.h"

MPU9250 mpu;
bool imu_ok = false;                // датчик знайдено на шині

void sensor_setup() {
  // Піни вказуємо явно і частота 400 кГц -- саме з цими параметрами датчик
  // налагоджено (node_common/raw_tests/raw_mpu9250).
  Wire.begin(21, 22);
  Wire.setClock(400000);
  Wire.setTimeOut(1000);            // без цього завислий I2C блокує весь loop()

  DBG.println("\nІніціалізація IMU MPU9250...");
  imu_ok = mpu.setup(0x68);
  if (!imu_ok) {
    DBG.println("[ПОМИЛКА] IMU не знайдено! Телеметрія піде з нульовими roll/pitch/yaw.");
    return;
  }
  DBG.println("[OK] IMU успішно підключено.");

  // Калібрування зсуву нуля акселерометра й гіроскопа: датчик має стояти
  // нерухомо ~кілька секунд, інакше зсув запишеться хибним.
  DBG.println("Калібрування акселерометра й гіроскопа -- НЕ ЧІПАЙ ДАТЧИК!");
  delay(2000);
  mpu.calibrateAccelGyro();
  DBG.println("Калібрування завершено.");
}

// Фільтр орієнтації бібліотеки рахує кути лише коли його оновлюють часто,
// тому update() викликаємо на кожному проході loop(), а не раз на 5 с.
void sensor_update() {
  if (imu_ok) mpu.update();
}

bool sensor_payload(char *buf, size_t n) {
  // Без датчика шлемо нулі (як раніше), щоб вузол лишався видимим у вебі.
  snprintf(buf, n, "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
           mpu.getRoll(), mpu.getPitch(), mpu.getYaw());
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }

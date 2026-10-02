/*
 * node_sht41.ino -- вузол з датчиком температури й вологості SHT41 (I2C, 0x44).
 *
 * Уся робота зі зв'язком (канали UART>TCP>UDP, буфер, ALARM з ACK/retry,
 * автоналаштування від шлюзу) -- у спільному node_common.h. Тут лише датчик:
 * драйвер SHT41 і payload {"temperature","humidity"}. Додаткових бібліотек
 * не потрібно, драйвер працює поверх Wire.
 *
 * Підключення: SDA/SCL за замовчуванням (GPIO21/22 на класичній ESP32).
 *
 * Поруч мають лежати копії node_common.h, packet_queue.h, protocol.*, reliability.*
 * (node_common/sync.sh розкладає їх автоматично).
 */
#include "node_common.h"

#define SHT_ADDR  0x44
#define CMD_MEAS  0xFD              // вимір високої точності, без нагріву

// CRC-8 (поліном 0x31, init 0xFF) з даташита: кожні 2 байти даних датчик
// супроводжує контрольним байтом.
uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }
  return crc;
}

// М'який ресет датчика -- виводить його зі стану "посеред виміру". Без цього
// після перезапуску плати перший вимір міг не пройти (потрібно було 2 ресети).
void shtReset() {
  Wire.beginTransmission(SHT_ADDR);
  Wire.write(0x89);
  Wire.endTransmission();
  delay(10);
}

// Один вимір із трьома спробами: I2C-шина й датчик інколи відповідають зі збоєм
// (NACK, неповна відповідь, помилка CRC), а наступна спроба зазвичай проходить.
bool readSHT41(float &temperature, float &humidity) {
  for (int attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(SHT_ADDR);
    Wire.write(CMD_MEAS);
    if (Wire.endTransmission() != 0) { delay(10); continue; }

    delay(10);                      // час виміру за даташитом ~8.3 мс

    uint8_t buf[6];
    if (Wire.requestFrom(SHT_ADDR, (uint8_t)6) != 6) { delay(10); continue; }
    for (int i = 0; i < 6; i++) buf[i] = Wire.read();

    if (crc8(buf, 2) != buf[2])     { delay(10); continue; }
    if (crc8(buf + 3, 2) != buf[5]) { delay(10); continue; }

    uint16_t rawT = buf[0] << 8 | buf[1];
    uint16_t rawH = buf[3] << 8 | buf[4];

    temperature = -45.0 + 175.0 * rawT / 65535.0;
    humidity    =  -6.0 + 125.0 * rawH / 65535.0;
    // Формула з даташита може трохи вийти за межі фізичної шкали
    if (humidity > 100.0) humidity = 100.0;
    if (humidity < 0.0)   humidity = 0.0;
    return true;
  }
  return false;
}

void sensor_setup() {
  // Wire.begin() БЕЗ параметрів і 100 кГц -- саме з цим датчик налагоджено
  // (node_common/raw_tests/raw_sht41); інші піни/частота давали збої.
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeOut(1000);            // без цього завислий I2C блокує весь loop()

  DBG.println("\nІніціалізація SHT41...");
  shtReset();
}

// SHT41 міряється повільно (десятки мс із ретраями) і лише на момент відправки,
// тож безперервне опитування не потрібне.
void sensor_update() {}

bool sensor_payload(char *buf, size_t n) {
  float t, h;
  if (!readSHT41(t, h)) {
    shtReset();                     // вивести датчик із завислого стану до наступного виміру
    return false;
  }
  snprintf(buf, n, "{\"temperature\":%.2f,\"humidity\":%.2f}", t, h);
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }

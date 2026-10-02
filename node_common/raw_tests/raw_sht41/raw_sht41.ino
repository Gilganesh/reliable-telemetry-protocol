#include <Wire.h>

#define SHT_ADDR  0x44
#define CMD_MEAS  0xFD

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

// м'який ресет датчика — виводить його зі стану "середині виміру"
void shtReset() {
  Wire.beginTransmission(SHT_ADDR);
  Wire.write(0x89);
  Wire.endTransmission();
  delay(10);
}

bool readSHT41(float &temperature, float &humidity) {
  for (int attempt = 0; attempt < 3; attempt++) {   // <-- ретраї
    Wire.beginTransmission(SHT_ADDR);
    Wire.write(CMD_MEAS);
    if (Wire.endTransmission() != 0) { delay(10); continue; }

    delay(10);

    uint8_t buf[6];
    if (Wire.requestFrom(SHT_ADDR, (uint8_t)6) != 6) { delay(10); continue; }
    for (int i = 0; i < 6; i++) buf[i] = Wire.read();

    if (crc8(buf, 2) != buf[2])     { delay(10); continue; }
    if (crc8(buf + 3, 2) != buf[5]) { delay(10); continue; }

    uint16_t rawT = buf[0] << 8 | buf[1];
    uint16_t rawH = buf[3] << 8 | buf[4];

    temperature = -45.0 + 175.0 * rawT / 65535.0;
    humidity    =  -6.0 + 125.0 * rawH / 65535.0;
    if (humidity > 100.0) humidity = 100.0;
    if (humidity < 0.0)   humidity = 0.0;
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== SHT41 ===");

  Wire.begin();          // БЕЗ параметрів — як у робочому варіанті
  Wire.setClock(100000);

  shtReset();            // <-- головне ліки від "потрібно 2 ресети"
}

void loop() {
  float t, h;
  if (readSHT41(t, h)) {
    Serial.printf("Температура: %.2f °C  |  Вологість: %.2f %%\n", t, h);
  } else {
    Serial.println("Помилка читання SHT41 — ресет");
    shtReset();
  }
  delay(1000);
}

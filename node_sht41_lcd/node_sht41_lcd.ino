#define NODE_LCD 1
#define NODE_I2C_SDA 21
#define NODE_I2C_SCL 22
#include "node_common.h"
#include "threshold.h"

#define TEMP_ALARM_C 35.0f
#define TEMP_ALARM_HYSTERESIS_C 2.0f

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

void shtReset() {
  Wire.beginTransmission(SHT_ADDR);
  Wire.write(0x89);
  Wire.endTransmission();
  delay(10);
}

bool readSHT41(float &temperature, float &humidity) {
  for (int attempt = 0; attempt < 3; attempt++) {
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

float last_temperature = 0;
float last_humidity = 0;
bool have_reading = false;
bool last_read_failed = false;
ThresholdAlarm overheat;
bool alarm_to_send = false;
char alarm_json[MAX_PAYLOAD_SIZE + 1];

void sensor_setup() {
  Wire.begin(NODE_I2C_SDA, NODE_I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(1000);
  lcd.setBusClocks(100000, 100000);
  lcd_init();
  threshold_init(&overheat);

  DBG.println("\nInitializing SHT41...");
  shtReset();
}

void sensor_update() {
  if (!alarm_to_send) return;
  alarm_to_send = false;
  send_alarm_json(alarm_json);
}

bool sensor_payload(char *buf, size_t n) {
  float t, h;
  if (!readSHT41(t, h)) {
    last_read_failed = true;
    shtReset();
    return false;
  }
  last_read_failed = false;
  have_reading = true;
  last_temperature = t;
  last_humidity = h;
  if (threshold_update(&overheat, t, TEMP_ALARM_C, TEMP_ALARM_HYSTERESIS_C)) {
    snprintf(alarm_json, sizeof(alarm_json), "{\"alarm\":\"overheat\",\"temperature\":%.1f,\"threshold\":%.0f}",
             t, (double)TEMP_ALARM_C);
    alarm_to_send = true;
    DBG.println("[ALARM] Temperature threshold exceeded.");
  }
  snprintf(buf, n, "{\"temperature\":%.2f,\"humidity\":%.2f}", t, h);
  return true;
}

bool sensor_display(char *line, size_t n) {
  if (last_read_failed) snprintf(line, n, "SHT41 error");
  else if (!have_reading) snprintf(line, n, "Measuring...");
  else if (overheat.active) snprintf(line, n, "T%.1fC ALARM!", last_temperature);
  else snprintf(line, n, "T%.1fC H%.1f%%", last_temperature, last_humidity);
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }

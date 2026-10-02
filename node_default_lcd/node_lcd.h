#ifndef NODE_LCD_H
#define NODE_LCD_H

#include <Arduino.h>
#include <Wire.h>

#define LCD_COLS 16
#define LCD_ROWS 2

class Lcd1602 {
 public:
  bool begin(uint8_t address = 0) {
    ready_ = false;
    address_ = 0;
    if (address != 0) {
      if (probe(address)) address_ = address;
    } else {
      static const uint8_t candidates[] = {0x27, 0x3F};
      for (uint8_t i = 0; i < sizeof(candidates); i++) {
        if (probe(candidates[i])) {
          address_ = candidates[i];
          break;
        }
      }
    }
    if (address_ == 0) return false;

    enter();
    bool ok = true;
    delay(50);
    ok &= nibble(0x3);
    delay(5);
    ok &= nibble(0x3);
    delay(5);
    ok &= nibble(0x3);
    delayMicroseconds(150);
    ok &= nibble(0x2);
    delayMicroseconds(150);
    ok &= command(0x28);
    ok &= command(0x08);
    ok &= command(0x01);
    delay(2);
    ok &= command(0x06);
    ok &= command(0x0C);
    leave();
    ready_ = ok;
    return ok;
  }

  bool ready() const { return ready_; }
  uint8_t address() const { return address_; }

  void setBusClocks(uint32_t lcd_hz, uint32_t bus_hz) {
    lcd_hz_ = lcd_hz;
    bus_hz_ = bus_hz;
  }

  void printLine(uint8_t row, const char *text) {
    if (!ready_ || row >= LCD_ROWS) return;
    uint8_t frames[LCD_COLS * 4];
    bool end = false;
    for (uint8_t i = 0; i < LCD_COLS; i++) {
      char c = ' ';
      if (!end && text[i] != '\0') c = text[i];
      else end = true;
      encode(static_cast<uint8_t>(c), true, frames + i * 4);
    }
    enter();
    bool ok = command(0x80 | (row == 0 ? 0x00 : 0x40));
    ok &= write(frames, sizeof(frames));
    leave();
    if (!ok) ready_ = false;
  }

  void clear() {
    if (!ready_) return;
    enter();
    bool ok = command(0x01);
    delay(2);
    leave();
    if (!ok) ready_ = false;
  }

 private:
  static const uint8_t RS = 0x01;
  static const uint8_t EN = 0x04;
  static const uint8_t BACKLIGHT = 0x08;

  uint8_t address_ = 0;
  bool ready_ = false;
  uint32_t lcd_hz_ = 0;
  uint32_t bus_hz_ = 0;

  bool probe(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
  }

  void enter() {
    if (lcd_hz_ != 0 && bus_hz_ != 0 && lcd_hz_ != bus_hz_) Wire.setClock(lcd_hz_);
  }

  void leave() {
    if (lcd_hz_ != 0 && bus_hz_ != 0 && lcd_hz_ != bus_hz_) Wire.setClock(bus_hz_);
  }

  bool write(const uint8_t *data, size_t n) {
    Wire.beginTransmission(address_);
    Wire.write(data, n);
    return Wire.endTransmission() == 0;
  }

  static void encode(uint8_t value, bool rs, uint8_t *out) {
    uint8_t mode = BACKLIGHT | (rs ? RS : 0);
    uint8_t hi = (value & 0xF0) | mode;
    uint8_t lo = static_cast<uint8_t>((value << 4) & 0xF0) | mode;
    out[0] = hi | EN;
    out[1] = hi;
    out[2] = lo | EN;
    out[3] = lo;
  }

  bool nibble(uint8_t value) {
    uint8_t d = static_cast<uint8_t>((value << 4) & 0xF0) | BACKLIGHT;
    uint8_t frame[2] = {static_cast<uint8_t>(d | EN), d};
    return write(frame, sizeof(frame));
  }

  bool command(uint8_t value) {
    uint8_t frame[4];
    encode(value, false, frame);
    return write(frame, sizeof(frame));
  }
};

#endif

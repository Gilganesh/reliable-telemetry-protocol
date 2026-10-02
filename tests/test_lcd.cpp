#include <stdio.h>
#include <string.h>
#include <string>
#include "Wire.h"
#include "node_lcd.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        printf("  [FAIL] %s\n", msg); \
    } else { \
        printf("  [ok]   %s\n", msg); \
    } \
} while (0)

class Hd44780OnPcf8574 : public WireDevice {
 public:
  explicit Hd44780OnPcf8574(uint8_t address) : address_(address) { memset(ddram, ' ', sizeof(ddram)); }

  uint8_t address() const override { return address_; }

  bool receive(uint8_t byte) override {
    if (!(byte & 0x08)) backlight_always_on = false;
    bool en = byte & 0x04;
    if (prev_en_ && !en) latch((byte >> 4) & 0x0F, byte & 0x01);
    prev_en_ = en;
    return true;
  }

  std::string row(int r) const { return std::string(ddram + (r == 0 ? 0 : 0x40), 16); }

  bool mode4 = false;
  bool function_ok = false;
  bool display_on = false;
  bool entry_increment = false;
  bool backlight_always_on = true;
  int protocol_errors = 0;
  char ddram[128];

 private:
  uint8_t address_;
  bool prev_en_ = false;
  int threes_ = 0;
  bool have_hi_ = false;
  uint8_t hi_ = 0;
  int addr_ = 0;

  void latch(uint8_t nib, bool rs) {
    if (!mode4) {
      if (!rs && nib == 0x3) { threes_++; return; }
      if (!rs && nib == 0x2 && threes_ >= 3) { mode4 = true; return; }
      protocol_errors++;
      return;
    }
    if (!have_hi_) { hi_ = nib; have_hi_ = true; return; }
    have_hi_ = false;
    byte_in(static_cast<uint8_t>((hi_ << 4) | nib), rs);
  }

  void byte_in(uint8_t v, bool rs) {
    if (rs) {
      if (addr_ >= 0 && addr_ < 128) ddram[addr_] = static_cast<char>(v);
      addr_ += entry_increment ? 1 : -1;
    } else if (v == 0x01) {
      memset(ddram, ' ', sizeof(ddram));
      addr_ = 0;
    } else if (v & 0x80) {
      addr_ = v & 0x7F;
    } else if (v == 0x06) {
      entry_increment = true;
    } else if ((v & 0xF8) == 0x08) {
      display_on = v & 0x04;
    } else if (v == 0x28) {
      function_ok = true;
    } else {
      protocol_errors++;
    }
  }
};

static void reset_bus() {
  Wire.devices.clear();
  Wire.clock_calls.clear();
  Wire.nack_writes = false;
}

static void test_init_finds_0x3f_and_configures() {
  printf("test_init_finds_0x3f_and_configures:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x3F);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  CHECK(lcd.begin(), "begin succeeds");
  CHECK(lcd.address() == 0x3F, "address 0x3F is detected");
  CHECK(dev.mode4 && dev.function_ok, "controller switched to 4-bit, 2-line mode");
  CHECK(dev.display_on && dev.entry_increment, "display on, cursor increments");
  CHECK(dev.protocol_errors == 0, "no protocol errors during init");
}

static void test_prefers_0x27() {
  printf("test_prefers_0x27:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x27);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  CHECK(lcd.begin() && lcd.address() == 0x27, "address 0x27 is detected");
}

static void test_missing_display() {
  printf("test_missing_display:\n");
  reset_bus();
  Lcd1602 lcd;
  CHECK(!lcd.begin(), "begin fails when nothing answers");
  CHECK(!lcd.ready(), "driver stays not ready");
  lcd.printLine(0, "ignored");
  CHECK(!lcd.ready(), "printing while not ready is a no-op");
}

static void test_print_lines() {
  printf("test_print_lines:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x27);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  lcd.begin();
  lcd.printLine(0, "UART    Node 2");
  lcd.printLine(1, "R-1 P-24 Y-11");
  CHECK(dev.row(0) == "UART    Node 2  ", "line 1 is padded to 16 columns");
  CHECK(dev.row(1) == "R-1 P-24 Y-11   ", "line 2 is written at 0x40");
  lcd.printLine(0, "0123456789ABCDEFGHIJ");
  CHECK(dev.row(0) == "0123456789ABCDEF", "text longer than 16 is truncated");
  lcd.printLine(0, "short");
  CHECK(dev.row(0) == "short           ", "shorter text erases the previous content");
  CHECK(dev.row(1) == "R-1 P-24 Y-11   ", "other line is untouched");
  CHECK(dev.backlight_always_on, "backlight bit is set in every byte");
  CHECK(dev.protocol_errors == 0, "no protocol errors");
}

static void test_clear() {
  printf("test_clear:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x27);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  lcd.begin();
  lcd.printLine(0, "hello");
  lcd.clear();
  CHECK(dev.row(0) == "                ", "clear blanks the display");
}

static void test_bus_error_marks_not_ready() {
  printf("test_bus_error_marks_not_ready:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x27);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  lcd.begin();
  Wire.nack_writes = true;
  lcd.printLine(0, "x");
  CHECK(!lcd.ready(), "a failed transfer marks the display not ready");
  Wire.nack_writes = false;
  CHECK(lcd.begin() && lcd.ready(), "begin recovers once the display answers again");
}

static void test_clock_switching() {
  printf("test_clock_switching:\n");
  reset_bus();
  Hd44780OnPcf8574 dev(0x27);
  Wire.devices.push_back(&dev);
  Lcd1602 lcd;
  lcd.setBusClocks(100000, 400000);
  lcd.begin();
  Wire.clock_calls.clear();
  lcd.printLine(0, "x");
  CHECK(Wire.clock_calls.size() == 2 && Wire.clock_calls[0] == 100000 && Wire.clock_calls[1] == 400000,
        "bus drops to the display speed for a write and returns to the sensor speed");

  reset_bus();
  Hd44780OnPcf8574 dev2(0x27);
  Wire.devices.push_back(&dev2);
  Lcd1602 same;
  same.setBusClocks(100000, 100000);
  same.begin();
  same.printLine(0, "x");
  CHECK(Wire.clock_calls.empty(), "no clock changes when both speeds are equal");
}

int main(void) {
  test_init_finds_0x3f_and_configures();
  test_prefers_0x27();
  test_missing_display();
  test_print_lines();
  test_clear();
  test_bus_error_marks_not_ready();
  test_clock_switching();

  printf("\n----------------------------------------\n");
  printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
  if (tests_failed == 0) printf("ALL TESTS PASSED\n");
  return tests_failed == 0 ? 0 : 1;
}

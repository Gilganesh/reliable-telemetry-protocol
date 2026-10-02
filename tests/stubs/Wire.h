#ifndef STUB_WIRE_H
#define STUB_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "Arduino.h"

struct WireDevice {
  virtual ~WireDevice() {}
  virtual uint8_t address() const = 0;
  virtual bool receive(uint8_t byte) = 0;
};

class TwoWire {
 public:
  std::vector<WireDevice *> devices;
  std::vector<uint32_t> clock_calls;
  bool nack_writes = false;

  void begin() {}
  void setClock(uint32_t hz) { clock_calls.push_back(hz); }

  void beginTransmission(uint8_t address) {
    target_ = nullptr;
    for (WireDevice *d : devices)
      if (d->address() == address) target_ = d;
    pending_.clear();
  }

  size_t write(uint8_t byte) {
    pending_.push_back(byte);
    return 1;
  }

  size_t write(const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) pending_.push_back(data[i]);
    return n;
  }

  uint8_t endTransmission() {
    if (!target_ || nack_writes) return 2;
    for (uint8_t b : pending_) target_->receive(b);
    return 0;
  }

 private:
  WireDevice *target_ = nullptr;
  std::vector<uint8_t> pending_;
};

inline TwoWire Wire;

#endif

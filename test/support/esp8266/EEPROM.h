#pragma once

#include <cstddef>
#include <cstdint>

// These methods only let the unrelated gateway_store implementation compile.
inline struct FakeEEPROM {
  void begin(size_t) {}
  template <typename T> void get(int, T& value) { value = {}; }
  template <typename T> void put(int, const T&) {}
  uint8_t read(int) { return 0; }
  void write(int, uint8_t) {}
  bool commit() { return true; }
} EEPROM;

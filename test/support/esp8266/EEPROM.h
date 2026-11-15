#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct FakeEEPROM {
  std::array<uint8_t, 88> persisted{};
  std::array<uint8_t, 88> staged{};
  bool commit_ok = true;
  bool available = true;
  size_t size = 0;
  unsigned commits = 0;
  void reset() {
    persisted.fill(0xff);
    staged.fill(0xff);
    commit_ok = true;
    available = true;
    size = 0;
    commits = 0;
  }
  void begin(size_t length) {
    size = available ? (length + 3) & ~size_t{3} : 0;
    staged = persisted;
  }
  template <typename T> void get(int address, T& value) {
    std::memcpy(&value, staged.data() + address, sizeof(value));
  }
  uint8_t read(int address) { return staged.at(static_cast<size_t>(address)); }
  void write(int address, uint8_t value) { staged.at(static_cast<size_t>(address)) = value; }
  const uint8_t* getConstDataPtr() const { return available ? staged.data() : nullptr; }
  size_t length() const { return size; }
  bool commit() {
    ++commits;
    if (!commit_ok) return false;
    persisted = staged;
    return true;
  }
};
inline FakeEEPROM EEPROM;

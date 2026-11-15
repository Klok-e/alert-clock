#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fake_esp8266 {
inline uint32_t now = 0;
inline bool connected = true;
inline std::vector<uint8_t> bytes;
inline std::vector<uint8_t> closing_tail;
inline size_t position = 0;
inline bool deliver_on_delay = false;

inline void reset() {
  now = 0;
  connected = true;
  bytes.clear();
  closing_tail.clear();
  position = 0;
  deliver_on_delay = false;
}

inline void deliver_tail_and_fin() {
  if (position == bytes.size() && !closing_tail.empty()) {
    bytes.insert(bytes.end(), closing_tail.begin(), closing_tail.end());
    closing_tail.clear();
    connected = false;
  }
}
}  // namespace fake_esp8266

inline uint32_t millis() { return fake_esp8266::now; }
inline void delay(uint32_t ms) {
  fake_esp8266::now += ms;
  if (fake_esp8266::deliver_on_delay) {
    fake_esp8266::deliver_tail_and_fin();
  }
}
inline constexpr uint32_t RANDOM_REG32 = 0;
inline constexpr int WL_CONNECTED = 3;
inline struct FakeWiFi {
  int status() const { return WL_CONNECTED; }
} WiFi;

class WiFiClient {
 public:
  void setTimeout(uint32_t) {}
  bool connect(const char*, uint16_t) { return true; }
  void setNoDelay(bool) {}
  void stop() { fake_esp8266::connected = false; }
  size_t write(const uint8_t*, size_t length) { return length; }

  int available() {
    const int sampled = static_cast<int>(fake_esp8266::bytes.size() - fake_esp8266::position);
    // The SDK may yield after sampling availability, receiving data and FIN before returning zero.
    if (!fake_esp8266::deliver_on_delay) {
      fake_esp8266::deliver_tail_and_fin();
    }
    return sampled;
  }

  bool connected() {
    const bool sampled = fake_esp8266::connected;
    // Also exercise arrival between the transport's connection sample and nonblocking read.
    if (!fake_esp8266::deliver_on_delay) {
      fake_esp8266::deliver_tail_and_fin();
    }
    return sampled;
  }

  int read(uint8_t* output, size_t capacity) {
    const size_t count = std::min(capacity, fake_esp8266::bytes.size() - fake_esp8266::position);
    if (count == 0) {
      return 0;
    }
    std::memcpy(output, fake_esp8266::bytes.data() + fake_esp8266::position, count);
    fake_esp8266::position += count;
    return static_cast<int>(count);
  }
};

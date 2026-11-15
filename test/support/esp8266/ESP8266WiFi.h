#pragma once

#include <cstdint>

namespace fake_esp8266 {
inline uint32_t now = 0;
inline bool wifi_up = true;
inline void reset() { now = 0; wifi_up = true; }
}
inline uint32_t millis() { return fake_esp8266::now; }
inline constexpr uint32_t RANDOM_REG32 = 0;
inline constexpr int WL_CONNECTED = 3;
inline struct FakeWiFi {
  int status() const { return fake_esp8266::wifi_up ? WL_CONNECTED : 0; }
} WiFi;

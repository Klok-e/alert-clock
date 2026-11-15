#pragma once

#include <cstdint>

namespace core {

// Days from 1970-01-01 to the given proleptic Gregorian date.
int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day);

// Parses RFC 3339 timestamps: "2026-11-14T19:19:09Z", "2026-11-14T19:19:09.123456Z",
// "2026-11-14T21:19:09+02:00". Anything else returns false and leaves `unix_seconds` untouched.
bool parse_rfc3339(const char* text, int64_t& unix_seconds);

struct LocalTime {
  int32_t year = 1970;
  uint8_t month = 1;    // 1..12
  uint8_t day = 1;      // 1..31
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 4;  // 0 = Sunday
};

}  // namespace core

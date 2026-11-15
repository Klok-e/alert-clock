#include "time_util.h"

namespace core {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Reads exactly `count` digits starting at `p`.
bool read_number(const char*& p, int count, int32_t& out) {
  int32_t value = 0;
  for (int i = 0; i < count; ++i) {
    if (!is_digit(p[i])) {
      return false;
    }
    value = value * 10 + (p[i] - '0');
  }
  p += count;
  out = value;
  return true;
}

bool expect(const char*& p, char c) {
  if (*p != c) {
    return false;
  }
  ++p;
  return true;
}

bool is_leap(int32_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int32_t days_in_month(int32_t year, int32_t month) {
  static constexpr int8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap(year)) {
    return 29;
  }
  return kDays[month - 1];
}

}  // namespace

int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day) {
  // Howard Hinnant's algorithm.
  const int32_t y = year - (month <= 2 ? 1 : 0);
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t mp = month > 2 ? month - 3 : month + 9;
  const uint32_t doy = (153 * mp + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_rfc3339(const char* text, int64_t& unix_seconds) {
  if (text == nullptr) {
    return false;
  }
  const char* p = text;
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  if (!read_number(p, 4, year) || !expect(p, '-') || !read_number(p, 2, month) || !expect(p, '-') ||
      !read_number(p, 2, day)) {
    return false;
  }
  if (*p != 'T' && *p != 't' && *p != ' ') {
    return false;
  }
  ++p;
  if (!read_number(p, 2, hour) || !expect(p, ':') || !read_number(p, 2, minute) || !expect(p, ':') ||
      !read_number(p, 2, second)) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || hour > 23 ||
      minute > 59 || second > 60) {
    return false;
  }
  if (*p == '.') {
    ++p;
    int fraction_digits = 0;
    while (is_digit(*p)) {
      ++p;
      if (++fraction_digits > 9) {
        return false;
      }
    }
    if (fraction_digits == 0) {
      return false;
    }
  }
  int32_t offset_seconds = 0;
  if (*p == 'Z' || *p == 'z') {
    ++p;
  } else if (*p == '+' || *p == '-') {
    const int32_t sign = *p == '-' ? -1 : 1;
    ++p;
    int32_t offset_hours = 0;
    int32_t offset_minutes = 0;
    if (!read_number(p, 2, offset_hours) || !expect(p, ':') || !read_number(p, 2, offset_minutes) ||
        offset_hours > 23 || offset_minutes > 59) {
      return false;
    }
    offset_seconds = sign * (offset_hours * 3600 + offset_minutes * 60);
  } else {
    return false;
  }
  if (*p != '\0') {
    return false;
  }
  const int64_t days =
      days_from_civil(year, static_cast<uint32_t>(month), static_cast<uint32_t>(day));
  unix_seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
  return true;
}

}  // namespace core

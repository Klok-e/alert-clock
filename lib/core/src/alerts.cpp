#include "alerts.h"

namespace core {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

AlertLevel level_from_letter(char c) {
  switch (c) {
    case 'N':
      return AlertLevel::None;
    case 'P':
      return AlertLevel::Partial;
    case 'Y':
      return AlertLevel::Yellow;
    case 'R':
    case 'A':
      return AlertLevel::Red;
    default:
      return AlertLevel::Unknown;
  }
}

int rank(AlertLevel level) {
  switch (level) {
    case AlertLevel::Partial:
    case AlertLevel::Yellow:
      return 1;
    case AlertLevel::Red:
      return 2;
    default:
      return 0;
  }
}

}  // namespace

AlertLevel parse_iot_alert(const char* body, size_t length) {
  if (body == nullptr) {
    return AlertLevel::Unknown;
  }
  size_t begin = 0;
  size_t end = length;
  while (begin < end && is_space(body[begin])) {
    ++begin;
  }
  while (end > begin && is_space(body[end - 1])) {
    --end;
  }
  if (end - begin != 3 || body[begin] != '"' || body[begin + 2] != '"') {
    return AlertLevel::Unknown;
  }
  return level_from_letter(body[begin + 1]);
}

const char* alert_level_name(AlertLevel level) {
  switch (level) {
    case AlertLevel::None:
      return "none";
    case AlertLevel::Partial:
      return "partial";
    case AlertLevel::Yellow:
      return "yellow";
    case AlertLevel::Red:
      return "red";
    case AlertLevel::Unknown:
      break;
  }
  return "unknown";
}

bool is_alerting(AlertLevel level) { return rank(level) > 0; }

AlertChange AlertTracker::update(AlertLevel level) {
  if (level == AlertLevel::Unknown) {
    return AlertChange::None;
  }
  const AlertLevel previous = level_;
  level_ = level;
  if (previous == AlertLevel::Unknown) {
    return AlertChange::None;
  }
  const int before = rank(previous);
  const int after = rank(level);
  if (before == 0 && after > 0) {
    return AlertChange::Raised;
  }
  if (after > before) {
    return AlertChange::Escalated;
  }
  if (before > 0 && after == 0) {
    return AlertChange::Cleared;
  }
  return AlertChange::None;
}

}  // namespace core

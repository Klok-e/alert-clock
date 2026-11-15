#include "alerts.h"

namespace core {
namespace {

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

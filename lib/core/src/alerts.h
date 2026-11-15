#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

// alerts.in.ua v2 IoT statuses for one region.
enum class AlertLevel : uint8_t {
  Unknown,  // no valid data
  None,     // "N"
  Partial,  // "P": alert in part of the region
  Yellow,   // "Y": preliminary threat
  Red,      // "R" (and v1 "A"): air-raid alert
};

// Parses a v2 IoT body: a JSON string holding one status letter, e.g. "R".
AlertLevel parse_iot_alert(const char* body, size_t length);

const char* alert_level_name(AlertLevel level);
bool is_alerting(AlertLevel level);

enum class AlertChange : uint8_t { None, Raised, Escalated, Cleared };

// Turns successive levels into the events worth a sound. The first valid level
// after boot produces no event, so restarts stay silent.
class AlertTracker {
 public:
  AlertChange update(AlertLevel level);
  AlertLevel level() const { return level_; }

 private:
  AlertLevel level_ = AlertLevel::Unknown;
};

}  // namespace core

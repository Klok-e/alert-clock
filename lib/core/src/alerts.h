#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

// The home region's alert level, as the gateway computes it from NEPTUN (the values are
// also the wire encoding: gateway/PROTOCOL.md).
enum class AlertLevel : uint8_t {
  Unknown,  // no valid data
  None,     // no alert at home
  Partial,  // alerts elsewhere in the home oblast
  Yellow,   // threat warning at home
  Red,      // air-raid alert at home
};

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

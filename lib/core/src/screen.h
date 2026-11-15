#pragma once

#include <cstdint>

#include "alerts.h"

namespace core {

enum class Panel : uint8_t { Clock, Map };
enum class Banner : uint8_t { None, NoData, Partial, Yellow, Red };

const char* panel_name(Panel panel);
const char* banner_name(Banner banner);

struct Situation {
  AlertLevel alert = AlertLevel::Unknown;
  bool alert_fresh = false;       // alert status confirmed recently
  bool threats_fresh = false;     // threat feed confirmed recently
  uint16_t threats_in_watch = 0;  // non-advisory tracks inside the watch radius
  bool map_requested = false;     // the user asked for the map
};

struct ScreenPlan {
  Panel panel = Panel::Clock;
  Banner banner = Banner::NoData;
};

// The map takes over when tracks are near or the user asks for it. The banner never
// shows "all clear" without fresh alert data: stale data shows NoData instead.
ScreenPlan plan_screen(const Situation& situation);

}  // namespace core

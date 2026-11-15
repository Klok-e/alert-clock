#pragma once

#include <cstdint>

#include "screen.h"
#include "time_util.h"
#include "wire.h"

namespace core {

constexpr uint32_t kNever = UINT32_MAX;

// Everything a frame needs. Markers are already in screen pixels; the renderer only draws.
struct ViewModel {
  Panel panel = Panel::Clock;
  Banner banner = Banner::NoData;
  bool notice = false;  // the start-up notice replaces the panel
  bool time_valid = false;
  LocalTime local;
  bool threats_fresh = false;
  uint32_t threats_age_s = kNever;  // since the gateway's threats source last had good data
  uint32_t alert_age_s = kNever;
  const Snapshot* snapshot = nullptr;  // the last valid snapshot (empty before the first)
  const Basemap* basemap = nullptr;    // drawn only while valid()
};

}  // namespace core

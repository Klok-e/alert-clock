#pragma once

#include <cstdint>

#include <etl/vector.h>

#include "screen.h"
#include "threats.h"
#include "time_util.h"

namespace core {

constexpr int16_t kScreenPx = 240;
constexpr size_t kMaxRings = 3;
constexpr uint32_t kNever = UINT32_MAX;

struct ThreatMarker {
  ScreenPoint point;
  int16_t uncertainty_px = 0;  // 0 when exact or unknown
  int16_t heading_deg = -1;    // -1 when unknown
  ThreatKind kind = ThreatKind::Unknown;
  bool approximate = false;
  bool stale = false;
  bool advisory = false;
};

// Everything a frame needs, already projected to screen pixels. The renderer only draws.
struct ViewModel {
  Panel panel = Panel::Clock;
  Banner banner = Banner::NoData;
  bool time_valid = false;
  LocalTime local;
  etl::vector<ThreatMarker, kMaxNearbyThreats> markers;
  etl::vector<uint8_t, kMaxRings> ring_km;
  float px_per_km = 0.0f;
  uint16_t threats_in_watch = 0;
  uint16_t area_only_in_region = 0;
  float nearest_km = -1.0f;      // < 0 when nothing is in range
  bool threats_fresh = false;
  uint32_t threats_age_s = kNever;
  uint32_t alert_age_s = kNever;
};

}  // namespace core

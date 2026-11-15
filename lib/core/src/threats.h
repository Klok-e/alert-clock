#pragma once

#include <cstddef>
#include <cstdint>

#include <etl/vector.h>

#include "byte_source.h"
#include "geo.h"

namespace core {

enum class ThreatKind : uint8_t { Uav, Recon, Fpv, Missile, Ballistic, Kab, Aircraft, Unknown };

ThreatKind parse_threat_kind(const char* type);
const char* threat_kind_name(ThreatKind kind);

// Kept compact: two snapshots of these live in RAM for the whole run.
struct Threat {
  uint32_t id = 0;               // hash of the feed's track id
  LatLon position;               // as reported at observed_at
  float distance_km = 0.0f;      // from home to `position`
  float uncertainty_km = 0.0f;
  uint32_t observed_at = 0;      // unix seconds; 0 when unknown
  int16_t heading_deg = -1;      // 0..359, -1 when unknown
  uint16_t speed_kmh = 0;        // 0 when unknown
  ThreatKind kind = ThreatKind::Unknown;
  uint8_t group_size = 0;        // 0 when not reported
  bool approximate = false;      // positionQuality "approx"
  bool stale = false;            // status "stale"
  bool advisory = false;         // an observation, never a shelter signal
};

constexpr size_t kMaxNearbyThreats = 24;

struct ThreatSnapshot {
  etl::vector<Threat, kMaxNearbyThreats> nearby;  // closest first
  uint16_t total = 0;                // tracks in the feed
  uint16_t area_only = 0;            // tracks naming only a region, never plotted
  uint16_t area_only_in_region = 0;  // ...of which name the home region
  uint16_t rejected = 0;             // tracks with missing or invalid fields
  bool overflow = false;             // more tracks were in range than `nearby` holds

  void clear() { *this = ThreatSnapshot{}; }
};

struct ThreatQuery {
  LatLon home;
  float radius_km = 50.0f;
  const char* home_region = nullptr;  // e.g. "Київська область"; counts area-only tracks there
};

enum class ThreatParse : uint8_t { Ok, NoThreatsArray, Malformed, ElementTooLarge };
const char* threat_parse_name(ThreatParse result);

// Streams a NEPTUN /api/v1/threats body. Each array element is parsed on its own into a
// fixed arena, so memory use does not grow with the feed. Only tracks within
// query.radius_km of home are kept. `out` is cleared first and is complete only on Ok.
ThreatParse parse_neptun_threats(ByteSource& in, const ThreatQuery& query, ThreatSnapshot& out);

// Largest gap the clock extrapolates over; older reports stay where they were seen.
constexpr int64_t kMaxExtrapolationS = 300;

// Dead-reckons along heading and speed from observed_at to now.
LatLon predicted_position(const Threat& threat, int64_t now_unix);

// Diagnostics for the most recent parse.
struct ThreatParseStats {
  size_t filter_bytes = 0;        // arena bytes held by the field filter
  size_t arena_high_water = 0;    // filter plus the largest single track
  uint32_t arena_failures = 0;
};
ThreatParseStats threat_parse_stats();

}  // namespace core

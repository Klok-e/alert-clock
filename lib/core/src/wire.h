#pragma once

#include <cstddef>
#include <cstdint>

#include <etl/vector.h>

#include "alerts.h"
#include "sha256.h"

// The gateway protocol, version 2: gateway/PROTOCOL.md.
namespace core {

enum class ThreatKind : uint8_t { Uav, Recon, Fpv, Missile, Ballistic, Kab, Aircraft, Unknown };
const char* threat_kind_name(ThreatKind kind);

struct ScreenPoint {
  int16_t x = 0;
  int16_t y = 0;
};

constexpr size_t kMaxMarkers = 24;
constexpr size_t kMaxTrail = 6;
constexpr size_t kMaxRings = 3;
constexpr size_t kMaxSnapshotBytes = 1024;
constexpr size_t kMaxBasemapBytes = 16384;
constexpr int16_t kScreenPx = 240;
constexpr uint16_t kAgeUnknown = 0xFFFF;
constexpr uint32_t kDurationUnknown = 0xFFFFFFFF;

struct Marker {
  ScreenPoint point;
  int16_t heading_deg = -1;  // -1 when unknown
  ThreatKind kind = ThreatKind::Unknown;
  bool approximate = false;
  bool stale = false;
  bool advisory = false;  // an observation, never a shelter signal
  uint8_t uncertainty_px = 0;
  uint8_t trail_count = 0;  // trail[0, trail_count), oldest first
  ScreenPoint trail[kMaxTrail];
};

struct Snapshot {
  AlertLevel alert_level = AlertLevel::Unknown;
  bool alerts_stale = true;
  bool threats_stale = true;
  uint16_t alerts_age_s = kAgeUnknown;
  uint16_t threats_age_s = kAgeUnknown;
  uint32_t alert_duration_s = kDurationUnknown;  // since home's alert started, at sending
  uint32_t basemap_version = 0;  // 0: the gateway has no basemap
  float px_per_km = 4.0f;
  etl::vector<uint8_t, kMaxRings> ring_km;
  uint16_t threats_in_watch = 0;
  uint16_t area_only_in_region = 0;
  float nearest_km = -1.0f;  // < 0 when nothing is near
  ThreatKind nearest_kind = ThreatKind::Unknown;
  int16_t nearest_heading_deg = -1;
  bool nearest_advisory = false;
  bool nearest_stale = false;
  etl::vector<Marker, kMaxMarkers> markers;  // nearest first
};

enum class WireResult : uint8_t { Ok, BadMac, WrongVersion, WrongType, WrongNonce, Malformed };
const char* wire_result_name(WireResult result);

using GatewayKey = uint8_t[kSha256Bytes];

// Parses the key as 64 hex digits. False, and `out` unchanged, otherwise.
bool parse_key_hex(const char* text, GatewayKey& out);

// Checks and decodes a snapshot body. `out` changes only when the result is Ok.
WireResult decode_snapshot(const uint8_t* body, size_t length, const GatewayKey& key, uint32_t nonce,
                           Snapshot& out);

// A basemap body kept in its download buffer and decoded one row at a time.
class Basemap {
 public:
  // Checks the body (MAC, header, every row) and adopts it; `body` must outlive the Basemap.
  WireResult load(const uint8_t* body, size_t length, const GatewayKey& key, uint32_t nonce);
  void invalidate() { valid_ = false; }
  bool valid() const { return valid_; }
  uint32_t version() const { return valid_ ? version_ : 0; }

  // Calls fn(x, length, rgb565) for each run of row `y` (0..239).
  template <typename F>
  void for_each_run(int16_t y, F&& fn) const {
    if (!valid_ || y < 0 || y >= kScreenPx) {
      return;
    }
    const uint8_t* row = rows_ + offset(y);
    const uint8_t count = row[0];
    int16_t x = 0;
    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t length = row[1 + 2 * i];
      fn(x, static_cast<int16_t>(length), palette_[row[2 + 2 * i] & 0x0F]);
      x = static_cast<int16_t>(x + length);
    }
  }

 private:
  uint16_t offset(int16_t y) const;

  const uint8_t* offsets_ = nullptr;
  const uint8_t* rows_ = nullptr;
  uint16_t palette_[16] = {};
  uint32_t version_ = 0;
  bool valid_ = false;
};

}  // namespace core

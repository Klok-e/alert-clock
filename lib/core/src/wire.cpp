#include "wire.h"

namespace core {
namespace {

constexpr uint8_t kVersion = 4;
constexpr uint8_t kTypeSnapshot = 1;
constexpr uint8_t kTypeBasemap = 2;
constexpr size_t kHeaderBytes = 6;
constexpr size_t kSnapshotFixedBytes = 31;
constexpr size_t kMarkerBytes = 14;
constexpr size_t kTrailPointBytes = 4;
constexpr size_t kBasemapFixedBytes = 4 + 32 + 2 * kScreenPx + 2;
constexpr uint16_t kNoDistance = 0xFFFF;

// Bounds-checked little-endian reads; a read past the end sets `failed`.
class Reader {
 public:
  Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

  uint8_t u8() {
    if (position_ + 1 > length_) {
      failed = true;
      return 0;
    }
    return data_[position_++];
  }
  uint16_t u16() {
    const uint16_t low = u8();
    return static_cast<uint16_t>(low | (u8() << 8));
  }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  uint32_t u32() {
    const uint32_t low = u16();
    return low | (static_cast<uint32_t>(u16()) << 16);
  }
  void skip(size_t count) {
    if (position_ + count > length_) {
      failed = true;
      return;
    }
    position_ += count;
  }
  size_t position() const { return position_; }

  bool failed = false;

 private:
  const uint8_t* data_;
  size_t length_;
  size_t position_ = 0;
};

ThreatKind kind_from(uint8_t value) {
  return value <= static_cast<uint8_t>(ThreatKind::Unknown) ? static_cast<ThreatKind>(value) : ThreatKind::Unknown;
}

AlertLevel level_from(uint8_t value) {
  return value <= static_cast<uint8_t>(AlertLevel::Red) ? static_cast<AlertLevel>(value) : AlertLevel::Unknown;
}

// Checks the MAC, version, type and nonce; on Ok, `body_length` excludes the MAC.
WireResult check(const uint8_t* body, size_t length, const GatewayKey& key, uint32_t nonce, uint8_t type,
                 size_t& body_length) {
  if (length < kHeaderBytes + kSha256Bytes) {
    return WireResult::Malformed;
  }
  body_length = length - kSha256Bytes;
  uint8_t mac[kSha256Bytes];
  hmac_sha256(key, body, body_length, mac);
  if (!equal_digest(mac, body + body_length, kSha256Bytes)) {
    return WireResult::BadMac;
  }
  Reader header(body, body_length);
  if (header.u8() != kVersion) {
    return WireResult::WrongVersion;
  }
  if (header.u8() != type) {
    return WireResult::WrongType;
  }
  if (header.u32() != nonce) {
    return WireResult::WrongNonce;
  }
  return WireResult::Ok;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

bool parse_key_hex(const char* text, GatewayKey& out) {
  if (text == nullptr) {
    return false;
  }
  uint8_t key[kSha256Bytes];
  for (size_t i = 0; i < kSha256Bytes; ++i) {
    const int high = hex_value(text[2 * i]);
    const int low = high < 0 ? -1 : hex_value(text[2 * i + 1]);
    if (low < 0) {
      return false;
    }
    key[i] = static_cast<uint8_t>(high << 4 | low);
  }
  if (text[2 * kSha256Bytes] != '\0') {
    return false;
  }
  for (size_t i = 0; i < kSha256Bytes; ++i) {
    out[i] = key[i];
  }
  return true;
}

const char* threat_kind_name(ThreatKind kind) {
  switch (kind) {
    case ThreatKind::Uav:
      return "uav";
    case ThreatKind::Recon:
      return "recon";
    case ThreatKind::Fpv:
      return "fpv";
    case ThreatKind::Missile:
      return "missile";
    case ThreatKind::Ballistic:
      return "ballistic";
    case ThreatKind::Kab:
      return "kab";
    case ThreatKind::Aircraft:
      return "aircraft";
    case ThreatKind::Unknown:
      break;
  }
  return "unknown";
}

const char* wire_result_name(WireResult result) {
  switch (result) {
    case WireResult::Ok:
      return "ok";
    case WireResult::BadMac:
      return "bad_mac";
    case WireResult::WrongVersion:
      return "wrong_version";
    case WireResult::WrongType:
      return "wrong_type";
    case WireResult::WrongNonce:
      return "wrong_nonce";
    case WireResult::Malformed:
      return "malformed";
  }
  return "malformed";
}

WireResult decode_snapshot(const uint8_t* body, size_t length, const GatewayKey& key, uint32_t nonce,
                           Snapshot& out) {
  size_t body_length = 0;
  const WireResult checked = check(body, length, key, nonce, kTypeSnapshot, body_length);
  if (checked != WireResult::Ok) {
    return checked;
  }
  // First pass: the counts must account for the body exactly.
  Reader sizes(body, body_length);
  sizes.skip(kHeaderBytes + kSnapshotFixedBytes - 1);
  const uint8_t marker_count = sizes.u8();
  if (marker_count > kMaxMarkers) {
    return WireResult::Malformed;
  }
  for (uint8_t i = 0; i < marker_count && !sizes.failed; ++i) {
    sizes.skip(kMarkerBytes - 1);
    const uint8_t trail = sizes.u8();
    if (trail > kMaxTrail) {
      return WireResult::Malformed;
    }
    sizes.skip(trail * kTrailPointBytes);
  }
  if (sizes.failed || sizes.position() != body_length) {
    return WireResult::Malformed;
  }

  Reader in(body, body_length);
  in.skip(kHeaderBytes);
  Snapshot& s = out;
  s.alert_level = level_from(in.u8());
  const uint8_t flags = in.u8();
  s.alerts_stale = (flags & 1) != 0;
  s.threats_stale = (flags & 2) != 0;
  s.alerts_age_s = in.u16();
  s.threats_age_s = in.u16();
  s.alert_duration_s = in.u32();
  s.basemap_version = in.u32();
  s.px_per_km = static_cast<float>(in.u16()) / 100.0f;
  const uint8_t ring_count = in.u8();
  s.ring_km.clear();
  for (size_t i = 0; i < kMaxRings; ++i) {
    const uint8_t km = in.u8();
    if (i < ring_count && km > 0) {
      s.ring_km.push_back(km);
    }
  }
  s.threats_in_watch = in.u16();
  s.area_only_in_region = in.u16();
  const uint16_t nearest = in.u16();
  s.nearest_km = nearest == kNoDistance ? -1.0f : static_cast<float>(nearest) / 10.0f;
  s.nearest_kind = kind_from(in.u8());
  s.nearest_heading_deg = in.i16();
  const uint8_t nearest_flags = in.u8();
  s.nearest_advisory = (nearest_flags & 1) != 0;
  s.nearest_stale = (nearest_flags & 2) != 0;
  in.u8();  // marker count, checked above
  s.markers.clear();
  for (uint8_t i = 0; i < marker_count; ++i) {
    Marker m;
    m.point.x = in.i16();
    m.point.y = in.i16();
    m.anchor.x = in.i16();
    m.anchor.y = in.i16();
    m.heading_deg = in.i16();
    m.kind = kind_from(in.u8());
    const uint8_t marker_flags = in.u8();
    m.predicted = (marker_flags & 8) != 0;
    m.approximate = (marker_flags & 1) != 0;
    m.stale = (marker_flags & 2) != 0;
    m.advisory = (marker_flags & 4) != 0;
    m.uncertainty_px = in.u8();
    m.trail_count = in.u8();
    for (uint8_t t = 0; t < m.trail_count && t < kMaxTrail; ++t) {
      m.trail[t].x = in.i16();
      m.trail[t].y = in.i16();
    }
    s.markers.push_back(m);
  }
  return WireResult::Ok;
}

uint16_t Basemap::offset(int16_t y) const {
  const size_t i = 2 * static_cast<size_t>(y);
  return static_cast<uint16_t>(offsets_[i] | (offsets_[i + 1] << 8));
}

size_t Basemap::read_run(size_t position, uint8_t& length, uint8_t& index) const {
  if (position >= rows_length_) {
    return 0;
  }
  const uint8_t tag = rows_[position];
  index = tag & 0x0F;
  length = tag >> 4;
  if (length != 0) {
    return 1;
  }
  if (position + 1 >= rows_length_) {
    return 0;
  }
  length = rows_[position + 1];
  return length < 16 ? 0 : 2;  // shorter runs take the one-byte form
}

WireResult Basemap::load(const uint8_t* body, size_t length, const GatewayKey& key, uint32_t nonce) {
  valid_ = false;
  size_t body_length = 0;
  const WireResult checked = check(body, length, key, nonce, kTypeBasemap, body_length);
  if (checked != WireResult::Ok) {
    return checked;
  }
  Reader in(body, body_length);
  in.skip(kHeaderBytes);
  if (body_length < kHeaderBytes + kBasemapFixedBytes) {
    return WireResult::Malformed;
  }
  const uint32_t version = in.u32();
  for (uint16_t& color : palette_) {
    color = in.u16();
  }
  offsets_ = body + in.position();
  in.skip(2 * kScreenPx);
  const uint16_t rows_length = in.u16();
  rows_ = body + in.position();
  if (in.failed || in.position() + rows_length != body_length) {
    return WireResult::Malformed;
  }
  rows_length_ = rows_length;
  // Every row must lie inside the rows area and cover exactly 240 pixels.
  for (int16_t y = 0; y < kScreenPx; ++y) {
    size_t position = offset(y);
    if (position >= rows_length) {
      return WireResult::Malformed;
    }
    const uint8_t count = rows_[position++];
    int total = 0;
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t run_length = 0;
      uint8_t index = 0;
      const size_t used = read_run(position, run_length, index);
      if (used == 0) {
        return WireResult::Malformed;
      }
      position += used;
      total += run_length;
    }
    if (total != kScreenPx) {
      return WireResult::Malformed;
    }
  }
  version_ = version;
  valid_ = true;
  return WireResult::Ok;
}

}  // namespace core

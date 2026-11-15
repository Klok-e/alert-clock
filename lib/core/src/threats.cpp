#include "threats.h"

#include <cmath>
#include <cstring>

#include "json.h"
#include "json_arena.h"
#include "time_util.h"

namespace core {
namespace {

// Holds the field filter plus the filtered fields of one track. ArduinoJson's nodes hold
// pointers, so the PC build (8-byte pointers) needs about twice the clock's space.
// Sized from the measured high water (threat_parse_stats()) with headroom for long ids
// and region names.
constexpr size_t kParseArenaBytes = 1536 * (sizeof(void*) / 4);
constexpr float kMaxSpeedKmh = 20000.0f;
constexpr float kMaxUncertaintyKm = 500.0f;

ArenaAllocator<kParseArenaBytes> g_arena;
ThreatParseStats g_stats;

bool is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int next_non_space(ByteSource& in) {
  int c = 0;
  do {
    c = in.read();
  } while (is_space(c));
  return c;
}

// Advances past `token`. A naive scan is enough here: the key appears once, before the array.
bool skip_past(ByteSource& in, const char* token) {
  const size_t length = std::strlen(token);
  size_t matched = 0;
  for (;;) {
    const int c = in.read();
    if (c < 0) {
      return false;
    }
    if (c == static_cast<unsigned char>(token[matched])) {
      if (++matched == length) {
        return true;
      }
    } else {
      matched = (c == static_cast<unsigned char>(token[0])) ? 1 : 0;
    }
  }
}

uint32_t fnv1a(const char* text) {
  uint32_t hash = 2166136261u;
  for (const char* p = text; *p != '\0'; ++p) {
    hash ^= static_cast<unsigned char>(*p);
    hash *= 16777619u;
  }
  return hash;
}

bool streq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

float clamp(float v, float lo, float hi) {
  if (!(v >= lo)) {  // also catches NaN
    return lo;
  }
  return v > hi ? hi : v;
}

void build_filter(JsonDocument& filter) {
  filter["id"] = true;
  filter["type"] = true;
  filter["region"] = true;
  filter["lat"] = true;
  filter["lon"] = true;
  filter["heading"] = true;
  filter["velocity"]["speedKmh"] = true;
  filter["uncertaintyKm"] = true;
  filter["confirmedAt"] = true;
  filter["updatedAt"] = true;
  filter["count"] = true;
  filter["status"] = true;
  filter["positionQuality"] = true;
  filter["advisory"] = true;
  filter["areaOnly"] = true;
}

void insert_by_distance(ThreatSnapshot& out, const Threat& threat) {
  auto& nearby = out.nearby;
  size_t position = 0;
  while (position < nearby.size() && nearby[position].distance_km <= threat.distance_km) {
    ++position;
  }
  if (nearby.full()) {
    out.overflow = true;
    if (position == nearby.size()) {
      return;
    }
    nearby.pop_back();
  }
  nearby.insert(nearby.begin() + static_cast<std::ptrdiff_t>(position), threat);
}

// Fills `threat` from one filtered element. Returns false if it must be skipped as invalid.
bool read_threat(JsonObjectConst o, Threat& threat) {
  JsonVariantConst lat = o["lat"];
  JsonVariantConst lon = o["lon"];
  if (!lat.is<float>() || !lon.is<float>()) {
    return false;
  }
  threat.position.lat = lat.as<float>();
  threat.position.lon = lon.as<float>();
  if (!std::isfinite(threat.position.lat) || !std::isfinite(threat.position.lon) ||
      std::fabs(threat.position.lat) > 90.0f || std::fabs(threat.position.lon) > 180.0f) {
    return false;
  }
  threat.id = fnv1a(o["id"] | "");
  threat.kind = parse_threat_kind(o["type"] | "");
  JsonVariantConst heading = o["heading"];
  if (heading.is<float>()) {
    const float h = heading.as<float>();
    if (std::isfinite(h)) {
      const long degrees = std::lround(std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f));
      threat.heading_deg = static_cast<int16_t>(degrees % 360);
    }
  }
  threat.speed_kmh =
      static_cast<uint16_t>(std::lround(clamp(o["velocity"]["speedKmh"] | 0.0f, 0.0f, kMaxSpeedKmh)));
  threat.uncertainty_km = clamp(o["uncertaintyKm"] | 0.0f, 0.0f, kMaxUncertaintyKm);
  int64_t observed = 0;
  if (!parse_rfc3339(o["confirmedAt"] | "", observed) && !parse_rfc3339(o["updatedAt"] | "", observed)) {
    observed = 0;
  }
  threat.observed_at = (observed > 0 && observed <= static_cast<int64_t>(UINT32_MAX))
                           ? static_cast<uint32_t>(observed)
                           : 0;
  const int count = o["count"] | 0;
  threat.group_size = static_cast<uint8_t>(count < 0 ? 0 : (count > 255 ? 255 : count));
  threat.approximate = streq(o["positionQuality"] | "", "approx");
  threat.stale = streq(o["status"] | "", "stale");
  threat.advisory = o["advisory"] | false;
  return true;
}

}  // namespace

ThreatKind parse_threat_kind(const char* type) {
  if (type == nullptr) {
    return ThreatKind::Unknown;
  }
  if (streq(type, "uav")) return ThreatKind::Uav;
  if (streq(type, "recon")) return ThreatKind::Recon;
  if (streq(type, "fpv")) return ThreatKind::Fpv;
  if (streq(type, "missile")) return ThreatKind::Missile;
  if (streq(type, "ballistic")) return ThreatKind::Ballistic;
  if (streq(type, "kab")) return ThreatKind::Kab;
  if (streq(type, "mig31k")) return ThreatKind::Aircraft;
  return ThreatKind::Unknown;
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

const char* threat_parse_name(ThreatParse result) {
  switch (result) {
    case ThreatParse::Ok:
      return "ok";
    case ThreatParse::NoThreatsArray:
      return "no_threats_array";
    case ThreatParse::Malformed:
      return "malformed";
    case ThreatParse::ElementTooLarge:
      return "element_too_large";
  }
  return "malformed";
}

ThreatParse parse_neptun_threats(ByteSource& in, const ThreatQuery& query, ThreatSnapshot& out) {
  out.clear();
  g_stats = ThreatParseStats{};
  if (!skip_past(in, "\"threats\"") || next_non_space(in) != ':' || next_non_space(in) != '[') {
    return ThreatParse::NoThreatsArray;
  }

  g_arena.reset();
  JsonDocument filter(&g_arena);
  build_filter(filter);
  g_stats.filter_bytes = g_arena.mark();
  if (g_arena.failures() != 0) {
    return ThreatParse::ElementTooLarge;
  }
  const size_t element_mark = g_arena.mark();

  PushbackSource source(in);
  int c = next_non_space(source);
  if (c == ']') {
    return ThreatParse::Ok;
  }
  for (;;) {
    if (c != '{') {
      return ThreatParse::Malformed;
    }
    source.unread(c);
    ++out.total;

    ThreatParse element_result = ThreatParse::Ok;
    {
      JsonDocument element(&g_arena);
      const DeserializationError error =
          deserializeJson(element, source, DeserializationOption::Filter(filter));
      if (error == DeserializationError::NoMemory) {
        element_result = ThreatParse::ElementTooLarge;
      } else if (error) {
        element_result = ThreatParse::Malformed;
      } else {
        JsonObjectConst o = element.as<JsonObjectConst>();
        const char* status = o["status"] | "";
        if (streq(status, "resolved")) {
          // Gone from the sky; neither shown nor counted as invalid.
        } else if (o["areaOnly"] | false) {
          ++out.area_only;
          if (query.home_region != nullptr && streq(o["region"] | "", query.home_region)) {
            ++out.area_only_in_region;
          }
        } else {
          Threat threat;
          if (!read_threat(o, threat)) {
            ++out.rejected;
          } else {
            threat.distance_km = distance_km(query.home, threat.position);
            if (threat.distance_km <= query.radius_km) {
              insert_by_distance(out, threat);
            }
          }
        }
      }
    }
    // The element document is gone; drop its memory and keep the filter's.
    g_arena.rewind(element_mark);
    g_stats.arena_high_water = g_arena.high_water();
    g_stats.arena_failures = g_arena.failures();
    if (element_result != ThreatParse::Ok) {
      return element_result;  // the stream position is lost; give up on this response
    }

    c = next_non_space(source);
    if (c == ']') {
      return ThreatParse::Ok;
    }
    if (c != ',') {
      return ThreatParse::Malformed;
    }
    c = next_non_space(source);
  }
}

LatLon predicted_position(const Threat& threat, int64_t now_unix) {
  const int64_t observed = static_cast<int64_t>(threat.observed_at);
  if (threat.heading_deg < 0 || threat.speed_kmh == 0 || observed == 0 || now_unix <= observed) {
    return threat.position;
  }
  int64_t elapsed = now_unix - observed;
  if (elapsed > kMaxExtrapolationS) {
    elapsed = kMaxExtrapolationS;
  }
  const float km = static_cast<float>(threat.speed_kmh) * static_cast<float>(elapsed) / 3600.0f;
  return destination(threat.position, static_cast<float>(threat.heading_deg), km);
}

ThreatParseStats threat_parse_stats() { return g_stats; }

}  // namespace core

#include "geo.h"

#include <cmath>

namespace core {
namespace {

constexpr float kEarthRadiusKm = 6371.0f;
constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
// Same sphere as distance_km(), so the map's scale matches the distance radii.
constexpr float kKmPerDeg = kEarthRadiusKm * kDegToRad;
constexpr float kMaxPx = 30000.0f;
constexpr float kMinRadiusKm = 0.1f;

float clamp_unit(float v) {
  if (v < -1.0f) {
    return -1.0f;
  }
  return v > 1.0f ? 1.0f : v;
}

float normalize_degrees(float deg) {
  float d = std::fmod(deg, 360.0f);
  if (d < 0.0f) {
    d += 360.0f;
  }
  return d >= 360.0f ? 0.0f : d;
}

int16_t clamp_px(float v) {
  if (!(v > -kMaxPx)) {  // also catches NaN
    return static_cast<int16_t>(-kMaxPx);
  }
  if (v > kMaxPx) {
    return static_cast<int16_t>(kMaxPx);
  }
  return static_cast<int16_t>(std::lround(v));
}

}  // namespace

float distance_km(LatLon a, LatLon b) {
  const float lat1 = a.lat * kDegToRad;
  const float lat2 = b.lat * kDegToRad;
  const float half_dlat = (lat2 - lat1) * 0.5f;
  const float half_dlon = (b.lon - a.lon) * kDegToRad * 0.5f;
  const float s = std::sin(half_dlat);
  const float t = std::sin(half_dlon);
  const float h = s * s + std::cos(lat1) * std::cos(lat2) * t * t;
  return 2.0f * kEarthRadiusKm * std::asin(std::sqrt(clamp_unit(h)));
}

float bearing_deg(LatLon from, LatLon to) {
  const float lat1 = from.lat * kDegToRad;
  const float lat2 = to.lat * kDegToRad;
  const float dlon = (to.lon - from.lon) * kDegToRad;
  const float y = std::sin(dlon) * std::cos(lat2);
  const float x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dlon);
  return normalize_degrees(std::atan2(y, x) * kRadToDeg);
}

LatLon destination(LatLon from, float bearing, float km) {
  const float d = km / kEarthRadiusKm;
  const float b = bearing * kDegToRad;
  const float lat1 = from.lat * kDegToRad;
  const float lon1 = from.lon * kDegToRad;
  const float lat2 = std::asin(
      clamp_unit(std::sin(lat1) * std::cos(d) + std::cos(lat1) * std::sin(d) * std::cos(b)));
  const float lon2 = lon1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(lat1),
                                       std::cos(d) - std::sin(lat1) * std::sin(lat2));
  LatLon out;
  out.lat = lat2 * kRadToDeg;
  out.lon = normalize_degrees(lon2 * kRadToDeg + 180.0f) - 180.0f;
  return out;
}

LocalProjection::LocalProjection(LatLon center, float radius_km, int16_t size_px)
    : center_(center),
      px_per_km_((static_cast<float>(size_px) * 0.5f) /
                 (radius_km > kMinRadiusKm ? radius_km : kMinRadiusKm)),
      cos_lat_(std::cos(center.lat * kDegToRad)),
      half_px_(static_cast<float>(size_px) * 0.5f) {}

ScreenPoint LocalProjection::project(LatLon point) const {
  const float dx_km = (point.lon - center_.lon) * kKmPerDeg * cos_lat_;
  const float dy_km = (point.lat - center_.lat) * kKmPerDeg;
  ScreenPoint out;
  out.x = clamp_px(half_px_ + dx_km * px_per_km_);
  out.y = clamp_px(half_px_ - dy_km * px_per_km_);
  return out;
}

}  // namespace core

#pragma once

#include <cstdint>

namespace core {

struct LatLon {
  float lat = 0.0f;
  float lon = 0.0f;
};

struct ScreenPoint {
  int16_t x = 0;
  int16_t y = 0;
};

// Great-circle distance in kilometres.
float distance_km(LatLon a, LatLon b);

// Initial bearing from `from` to `to`, degrees clockwise from north, in [0, 360).
float bearing_deg(LatLon from, LatLon to);

// The point `km` kilometres from `from` along `bearing` degrees.
LatLon destination(LatLon from, float bearing, float km);

// Local equirectangular projection on the same sphere as distance_km(), centred on
// `center`: x grows east, y grows south.
// `radius_km` maps to half the screen. Error stays far below a pixel at the
// < 100 km radii this clock shows. A future basemap must use the same projection.
class LocalProjection {
 public:
  LocalProjection(LatLon center, float radius_km, int16_t size_px);

  // Coordinates far off screen are clamped to +/-30000.
  ScreenPoint project(LatLon point) const;
  float px_per_km() const { return px_per_km_; }

 private:
  LatLon center_;
  float px_per_km_;
  float cos_lat_;
  float half_px_;
};

}  // namespace core

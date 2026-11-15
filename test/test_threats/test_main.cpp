// Which tracks the clock keeps, and where it draws them.

#include <unity.h>

#include <cstdio>
#include <string>

#include "byte_source.h"
#include "geo.h"
#include "threats.h"

using namespace core;

namespace {

constexpr float kHomeLat = 50.4501f;
constexpr float kHomeLon = 30.5234f;
constexpr float kKmPerDeg = 111.19493f;  // 6371 km sphere, as distance_km() uses

// A NEPTUN-shaped track `km` due north of home, with fields the filter must skip.
std::string track_north(int index, float km) {
  char text[400];
  std::snprintf(text, sizeof(text),
                "{\"id\":\"trk_%d\",\"type\":\"uav\",\"title\":\"x\",\"lat\":%.5f,\"lon\":%.5f,"
                "\"heading\":180,\"velocity\":{\"bearingDeg\":180,\"speedKmh\":150},"
                "\"trail\":[{\"lat\":1,\"lon\":2,\"t\":\"x\"}],\"status\":\"active\","
                "\"confirmedAt\":\"2026-11-14T19:00:00Z\",\"positionQuality\":\"confirmed\"}",
                index, static_cast<double>(kHomeLat + km / kKmPerDeg), static_cast<double>(kHomeLon));
  return text;
}

}  // namespace

void setUp() {}
void tearDown() {}

// During a mass attack more tracks are in range than the clock can hold. It must keep
// the closest ones, nearest first, whatever order the feed lists them in.
void test_overflow_keeps_the_closest_tracks_nearest_first() {
  std::string body = "{\"epoch\":1,\"threats\": [ ";
  for (int km = 30; km >= 1; --km) {  // farthest first
    body += track_north(km, static_cast<float>(km));
    body += km > 1 ? " , " : " ";
  }
  body += "] }";
  MemorySource in(body.data(), body.size());
  ThreatQuery query;
  query.home = LatLon{kHomeLat, kHomeLon};
  query.radius_km = 50.0f;
  ThreatSnapshot snapshot;

  TEST_ASSERT_EQUAL(static_cast<int>(ThreatParse::Ok), static_cast<int>(parse_neptun_threats(in, query, snapshot)));
  TEST_ASSERT_EQUAL(30, snapshot.total);
  TEST_ASSERT_TRUE(snapshot.overflow);
  TEST_ASSERT_EQUAL(static_cast<int>(kMaxNearbyThreats), static_cast<int>(snapshot.nearby.size()));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.0f, snapshot.nearby.front().distance_km);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, static_cast<float>(kMaxNearbyThreats), snapshot.nearby.back().distance_km);
  for (size_t i = 1; i < snapshot.nearby.size(); ++i) {
    TEST_ASSERT_TRUE(snapshot.nearby[i - 1].distance_km <= snapshot.nearby[i].distance_km);
  }
}

// North is up and east is right, at the configured scale (30 km radius on 240 px = 4 px/km).
void test_projection_puts_north_up_and_east_right_at_scale() {
  const LatLon home{kHomeLat, kHomeLon};
  const LocalProjection projection(home, 30.0f, 240);
  const float km_per_deg_lon = kKmPerDeg * 0.63675f;  // cos(50.4501 deg)

  const ScreenPoint north = projection.project(LatLon{kHomeLat + 15.0f / kKmPerDeg, kHomeLon});
  const ScreenPoint east = projection.project(LatLon{kHomeLat, kHomeLon + 15.0f / km_per_deg_lon});

  TEST_ASSERT_INT_WITHIN(1, 120, north.x);
  TEST_ASSERT_INT_WITHIN(1, 60, north.y);
  TEST_ASSERT_INT_WITHIN(1, 180, east.x);
  TEST_ASSERT_INT_WITHIN(1, 120, east.y);
}

void test_distance_matches_a_known_city_pair() {
  // Kyiv to Lviv on a 6371 km sphere, computed independently with Python.
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 467.53f, distance_km(LatLon{50.4501f, 30.5234f}, LatLon{49.8397f, 24.0297f}));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_overflow_keeps_the_closest_tracks_nearest_first);
  RUN_TEST(test_projection_puts_north_up_and_east_right_at_scale);
  RUN_TEST(test_distance_matches_a_known_city_pair);
  return UNITY_END();
}

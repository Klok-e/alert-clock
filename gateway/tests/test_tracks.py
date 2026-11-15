import mock_server
from alert_gateway import tracks, geo


def area_only(track_id, region, region_key):
    return tracks.parse_track(mock_server.track(track_id, "uav", 0, 50, area_only=True, region=region,
                                                region_key=region_key))


def test_area_only_tracks_count_for_the_home_oblast_by_key_or_name():
    # NEPTUN's "region" wording varies; "regionKey" matches the region files' keys.
    feed = [
        area_only("by_key", "Kyiv region", "київська"),
        area_only("by_name", "Київська область", ""),
        area_only("elsewhere", "Чернігівська область", "чернігівська"),
    ]
    picture = tracks.build(feed, mock_server.HOME_VYSHHOROD, 30, 25, mock_server.OBSERVED_UNIX,
                           "київська", ("Київська область",))

    assert picture.area_only_in_region == 2
    assert picture.markers == []  # no position to draw


def test_motion_requires_active_confirmed_velocity_and_never_uses_updated_at():
    mutations = (
        lambda raw: raw.update(status="stale"),
        lambda raw: raw.update(confirmedAt=None),
        lambda raw: raw.pop("velocity"),
        lambda raw: raw["velocity"].pop("bearingDeg"),
        lambda raw: raw["velocity"].pop("speedKmh"),
    )
    for mutate in mutations:
        raw = mock_server.track("motion", "uav", 0, 2, heading=90, speed=120)
        mutate(raw)
        raw["updatedAt"] = mock_server.iso(mock_server.OBSERVED_UNIX - 3600)
        parsed = tracks.parse_track(raw)
        assert tracks.predicted(parsed, mock_server.OBSERVED_UNIX + 300) == (parsed.lat, parsed.lon)


def test_kind_specific_motion_caps_limit_time_and_distance():
    # With 1,200 km/h, the time caps would exceed each distance cap; both bounds matter.
    caps = {
        "uav": (12, 18), "recon": (12, 12), "fpv": (10, 10), "missile": (5, 30),
        "kab": (4, 10), "ballistic": (1.5, 20), "aircraft": (6, 24), "unknown": (6, 10),
    }
    for kind, (minutes, distance_cap) in caps.items():
        slow = tracks.parse_track(mock_server.track(kind, kind, 0, 1, heading=90, speed=60))
        slow_position = tracks.predicted(slow, mock_server.OBSERVED_UNIX + 3600)
        assert abs(geo.distance_km((slow.lat, slow.lon), slow_position) - minutes) < 0.1

        fast = tracks.parse_track(mock_server.track(kind, kind, 0, 1, heading=90, speed=1200))
        fast_position = tracks.predicted(fast, mock_server.OBSERVED_UNIX + 3600)
        assert abs(geo.distance_km((fast.lat, fast.lon), fast_position) - distance_cap) < 0.1


def test_trail_keeps_recent_history_evenly_sampled_and_separate_from_prediction():
    raw = mock_server.track("trail", "uav", 0, 2, heading=90, speed=100)
    reported = (raw["lat"], raw["lon"])
    raw["trail"] = [
        {"lat": mock_server.HOME[0], "lon": mock_server.HOME[1] + i / 100,
         "t": mock_server.iso(mock_server.OBSERVED_UNIX - 600 + i * 30)}
        for i in range(20)
    ]
    parsed = tracks.parse_track(raw)
    picture = tracks.build([parsed], mock_server.HOME, 30, 25,
                           mock_server.OBSERVED_UNIX + 300, None, ())
    marker = picture.markers[0]
    projection = geo.Projection(mock_server.HOME, 30)

    assert len(marker.trail) == 6
    all_points = [projection.project((p["lat"], p["lon"])) for p in raw["trail"]]
    selected = [all_points.index(p) for p in marker.trail]
    assert selected[0] == 0
    assert selected[-1] == 19
    assert all(3 <= right - left <= 4 for left, right in zip(selected, selected[1:]))
    assert marker.anchor == projection.project(reported)
    assert marker.anchor != (marker.x, marker.y)

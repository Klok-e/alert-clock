import mock_server
from alert_gateway import tracks


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

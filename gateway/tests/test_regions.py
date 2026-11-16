import copy
from datetime import datetime, timezone

import mock_server
from alert_gateway import regions, wire


def red(key, name, oblast):
    return mock_server.entry(key, name, oblast, "red")


def test_kyiv_city_never_shows_partial_for_raion_alerts_in_kyiv_oblast():
    # Kyiv city lies inside Kyiv oblast's outline, here without a hole cut for the city;
    # the oblast's raion alerts are not the city's.
    oblasts = copy.deepcopy(mock_server.OBLASTS)
    kyiv_oblast = next(f for f in oblasts["features"] if f["properties"]["key"] == "київська")
    del kyiv_oblast["geometry"]["coordinates"][1:]
    home = regions.resolve_home(oblasts, mock_server.RAIONS, *mock_server.HOME)
    alerts = {"raions": [red("броварський", "Броварський район", "Київська область")], "oblasts": []}

    assert home.oblast_key == "м. київ"
    assert regions.alert_level(alerts, home) == wire.LEVEL_NONE


def test_a_raion_alert_elsewhere_in_the_home_oblast_is_partial():
    home = regions.resolve_home(mock_server.OBLASTS, mock_server.RAIONS, *mock_server.HOME_VYSHHOROD)
    alerts = {"raions": [red("броварський", "Броварський район", "Київська область")], "oblasts": []}

    assert (home.oblast_key, home.raion_key) == ("київська", "вишгородський")
    assert regions.alert_level(alerts, home) == wire.LEVEL_PARTIAL


def test_an_oblast_entry_without_the_home_raion_is_partial_with_no_alert_start():
    # NEPTUN may alert an oblast while excluding some raions: only the home raion's own entry
    # alerts home, so an earlier, more severe oblast entry neither raises the level nor starts the clock.
    home = regions.resolve_home(mock_server.OBLASTS, mock_server.RAIONS, *mock_server.HOME_VYSHHOROD)
    red_oblast = red("київська", "Київська область", "Київська область")
    red_oblast["since"] = "2026-11-14T17:00:00Z"
    oblast_only = {"raions": [], "oblasts": [red_oblast]}
    yellow = mock_server.entry("вишгородський", "Вишгородський район", "Київська область", "yellow")
    yellow["since"] = "2026-11-14T18:00:00.250Z"
    with_raion = {"raions": [yellow], "oblasts": [red_oblast]}

    assert regions.alert_level(oblast_only, home) == wire.LEVEL_PARTIAL
    assert regions.alert_start(oblast_only, home) is None
    assert regions.alert_level(with_raion, home) == wire.LEVEL_YELLOW
    assert regions.alert_start(with_raion, home) == datetime(2026, 11, 14, 18, 0, 0, 250000, timezone.utc).timestamp()

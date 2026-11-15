import asyncio
import json

import pytest

import mock_server
from alert_gateway import neptun, regions, wire

VALID_YELLOW_ALERTS = {
    "raions": [mock_server.entry("вишгородський", "Вишгородський район", "Київська область", "yellow")],
    "oblasts": [],
}
MALFORMED_ALERT_CASES = [
    pytest.param(None, id="null-payload"),
    pytest.param([], id="list-payload"),
    pytest.param({"raions": [None], "oblasts": []}, id="null-entry"),
    pytest.param({"raions": [3], "oblasts": []}, id="scalar-entry"),
    pytest.param({"raions": [{"key": []}], "oblasts": []}, id="list-key"),
    pytest.param({"raions": [{"key": {}}], "oblasts": []}, id="object-key"),
    pytest.param({"raions": [{"key": None}], "oblasts": []}, id="null-key"),
    pytest.param({"raions": [{}], "oblasts": []}, id="missing-key"),
    pytest.param({"raions": [{"key": 17}], "oblasts": []}, id="numeric-key"),
    pytest.param(
        {"raions": VALID_YELLOW_ALERTS["raions"], "oblasts": [None]},
        id="valid-raions-invalid-oblasts",
    ),
    pytest.param({"oblasts": []}, id="missing-raions"),
    pytest.param({"raions": [], "oblasts": None}, id="null-oblasts"),
    pytest.param({"raions": {}, "oblasts": []}, id="object-raions"),
]


def feed_with_alert():
    clock = neptun.Clock()
    clock.set(1000)
    changes = []

    def changed():
        changes.append(regions.fills(feed.alerts))

    feed = neptun.Neptun(None, clock, changed)
    feed.handle_frame(json.dumps({"type": "alerts", "data": VALID_YELLOW_ALERTS}))
    feed.sources["alerts"].rest_ok = True
    clock.set(1100)
    return feed, clock, changes


@pytest.mark.parametrize("bad", MALFORMED_ALERT_CASES)
def test_malformed_stream_alerts_preserve_state_freshness_and_callback(bad, caplog):
    feed, clock, changes = feed_with_alert()
    feed.handle_frame(json.dumps({"type": "alerts", "data": bad}))
    assert feed.alerts == VALID_YELLOW_ALERTS
    assert feed.sources["alerts"].last_good == 1000
    assert feed.sources["alerts"].rest_ok is True
    assert len(changes) == 1
    assert "malformed alerts" in caplog.text
    feed.handle_frame(json.dumps({"type": "alerts", "data": {"raions": [], "oblasts": []}}))
    assert feed.alerts == {"raions": [], "oblasts": []}
    assert feed.sources["alerts"].last_good == 1100
    assert changes[-1] == ({}, {})


@pytest.mark.parametrize("bad", MALFORMED_ALERT_CASES)
def test_malformed_rest_alerts_disable_heartbeat_freshness_without_replacing_data(bad, caplog):
    feed, clock, changes = feed_with_alert()

    async def response(path):
        return bad

    feed._get_json = response
    assert asyncio.run(feed.fetch_source("alerts")) is False
    assert feed.alerts == VALID_YELLOW_ALERTS
    assert feed.sources["alerts"].last_good == 1000
    assert feed.sources["alerts"].rest_ok is False
    assert len(changes) == 1
    assert "REST alerts failed" in caplog.text
    clock.set(1200)
    feed.handle_frame(json.dumps({"type": "heartbeat"}))
    assert feed.sources["alerts"].stale(clock.now())


def test_valid_alerts_keep_metadata_conservative_defaults_and_rest_recovery():
    feed, clock, changes = feed_with_alert()
    home = regions.resolve_home(mock_server.OBLASTS, mock_server.RAIONS, *mock_server.HOME_VYSHHOROD)
    for level in (None, "unrecognised", 17):
        entry = {"key": "вишгородський", "since": ["unreadable"], "extra": {"preserved": True}}
        if level is not None:
            entry["level"] = level
        body = {"raions": [entry], "oblasts": []}
        feed.handle_frame(json.dumps({"type": "alerts", "data": body}))
        assert feed.alerts == body
        assert regions.alert_level(feed.alerts, home) == wire.LEVEL_RED
        assert regions.alert_start(feed.alerts, home) is None
        assert changes[-1] == ({}, {"вишгородський": wire.LEVEL_RED})
    feed.sources["alerts"].rest_ok = False

    async def response(path):
        return VALID_YELLOW_ALERTS

    feed._get_json = response
    clock.set(1300)
    before = len(changes)
    assert asyncio.run(feed.fetch_source("alerts")) is True
    assert feed.alerts == VALID_YELLOW_ALERTS
    assert feed.sources["alerts"].last_good == 1300
    assert feed.sources["alerts"].rest_ok is True
    assert len(changes) == before + 1

"""Home region lookup and the banner level from NEPTUN's /api/v1/alerts."""

from __future__ import annotations

from dataclasses import dataclass

from . import geo
from .tracks import parse_time
from .wire import LEVEL_NONE, LEVEL_PARTIAL, LEVEL_RED, LEVEL_YELLOW


KYIV_CITY = "м. київ"  # an oblast-level region with no raions; never Partial


@dataclass(frozen=True)
class HomeRegion:
    oblast_key: str
    oblast_region: str | None  # the oblast's "region" property, e.g. "Київська область"
    oblast_name: str
    raion_key: str | None  # None for Kyiv city, which has no raions

    @property
    def track_region_name(self) -> str:
        return self.oblast_region or self.oblast_name


def _area(geometry: dict) -> float:
    total = 0.0
    for polygon in geo.polygons(geometry):
        ring = polygon[0]
        total += abs(sum(ring[i][0] * ring[i - 1][1] - ring[i - 1][0] * ring[i][1] for i in range(len(ring)))) / 2
    return total


def _containing(features: list[dict], lat: float, lon: float) -> list[dict]:
    return [f for f in features if geo.contains(f["geometry"], lat, lon)]


def resolve_home(oblasts: dict, raions: dict, lat: float, lon: float) -> HomeRegion | None:
    """The smallest oblast-level feature containing home, and the raion containing it."""
    candidates = _containing(oblasts["features"], lat, lon)
    if not candidates:
        return None
    oblast = min(candidates, key=lambda f: _area(f["geometry"]))
    props = oblast["properties"]
    raion = None
    if props["key"] != KYIV_CITY:
        found = _containing(raions["features"], lat, lon)
        if found:
            raion = min(found, key=lambda f: _area(f["geometry"]))["properties"]["key"]
    return HomeRegion(props["key"], props.get("region"), props.get("name") or props.get("region") or props["key"],
                      raion)


def entry_level(entry: dict) -> int:
    """Yellow only when NEPTUN says so; anything else, missing included, counts as red."""
    return LEVEL_YELLOW if entry.get("level") == "yellow" else LEVEL_RED


def _home_entries(alerts: dict, home: HomeRegion) -> list[dict]:
    """The alert entries for home's raion and oblast."""
    entries = [e for e in alerts.get("raions", []) if home.raion_key and e.get("key") == home.raion_key]
    return entries + [e for e in alerts.get("oblasts", []) if e.get("key") == home.oblast_key]


def alert_level(alerts: dict, home: HomeRegion) -> int:
    levels = [entry_level(e) for e in _home_entries(alerts, home)]
    if levels:
        return max(levels)
    if home.oblast_key != KYIV_CITY and home.oblast_region and any(e.get("oblast") == home.oblast_region for e in alerts.get("raions", [])):
        return LEVEL_PARTIAL
    return LEVEL_NONE


def alert_start(alerts: dict, home: HomeRegion) -> float | None:
    """Unix time of the earliest `since` among home's entries, so an escalation from yellow to red
    keeps counting; None with no entry for home (Partial included) or no readable `since`."""
    starts = [t for t in (parse_time(e.get("since")) for e in _home_entries(alerts, home)) if t is not None]
    return min(starts) if starts else None


def fills(alerts: dict) -> tuple[dict[str, int], dict[str, int]]:
    """Fill level per oblast key and per raion key."""
    oblasts = {e["key"]: entry_level(e) for e in alerts.get("oblasts", []) if "key" in e}
    raions = {e["key"]: entry_level(e) for e in alerts.get("raions", []) if "key" in e}
    return oblasts, raions

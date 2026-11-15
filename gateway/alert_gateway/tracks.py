"""NEPTUN tracks turned into the snapshot's counts, nearest track and markers."""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import datetime

from . import geo
from .wire import KIND_UNKNOWN, KINDS, MAX_MARKERS, MAX_TRAIL, Marker

MAX_EXTRAPOLATION_S = 300
TRAIL_WINDOW_S = 300
OFFSCREEN_MARGIN_PX = 24
MAX_UNCERTAINTY_PX = 200
KEEP_FACTOR = 1.5  # tracks within 1.5 x the map radius count as nearby
_KIND_ALIASES = {"mig31k": "aircraft"}


def parse_time(text: object) -> float | None:
    if not isinstance(text, str) or not text:
        return None
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return None
    return float(value)


@dataclass
class Track:
    id: str
    kind: int
    lat: float
    lon: float
    heading: int
    speed_kmh: float
    observed: float | None
    uncertainty_km: float
    approximate: bool
    stale: bool
    advisory: bool
    area_only: bool
    region: str
    region_key: str
    trail: list[tuple[float, float, float]]


def parse_track(item: dict) -> Track | None:
    """None for resolved or unusable entries."""
    if not isinstance(item, dict) or item.get("status") == "resolved":
        return None
    lat, lon = _number(item.get("lat")), _number(item.get("lon"))
    if lat is None or lon is None or abs(lat) > 90 or abs(lon) > 180:
        return None
    kind_name = _KIND_ALIASES.get(item.get("type"), item.get("type"))
    velocity = item.get("velocity") if isinstance(item.get("velocity"), dict) else {}
    heading = _number(velocity.get("bearingDeg"))
    if heading is None:
        heading = _number(item.get("heading"))
    speed = _number(velocity.get("speedKmh")) or 0.0
    trail = []
    for point in item.get("trail") or []:
        if isinstance(point, dict):
            plat, plon, t = _number(point.get("lat")), _number(point.get("lon")), parse_time(point.get("t"))
            if plat is not None and plon is not None and t is not None:
                trail.append((plat, plon, t))
    return Track(
        id=str(item.get("id", "")),
        kind=KINDS.index(kind_name) if kind_name in KINDS else KIND_UNKNOWN,
        lat=lat, lon=lon,
        heading=-1 if heading is None else round(heading % 360) % 360,
        speed_kmh=max(0.0, min(speed, 20000.0)),
        observed=parse_time(item.get("confirmedAt")) or parse_time(item.get("updatedAt")),
        uncertainty_km=max(0.0, min(_number(item.get("uncertaintyKm")) or 0.0, 500.0)),
        approximate=item.get("positionQuality") == "approx",
        stale=item.get("status") == "stale",
        advisory=item.get("advisory") is True,
        area_only=item.get("areaOnly") is True,
        region=str(item.get("region") or ""),
        region_key=str(item.get("regionKey") or ""),
        trail=sorted(trail, key=lambda p: p[2]),
    )


def predicted(track: Track, now: float) -> tuple[float, float]:
    """Dead reckoning along heading and speed from the observed time, capped at 300 s."""
    if track.heading < 0 or track.speed_kmh <= 0 or not track.observed or now <= track.observed:
        return (track.lat, track.lon)
    elapsed = min(now - track.observed, MAX_EXTRAPOLATION_S)
    return geo.destination((track.lat, track.lon), track.heading, track.speed_kmh * elapsed / 3600)


@dataclass
class Picture:
    threats_in_watch: int = 0
    area_only_in_region: int = 0
    nearest_km: float | None = None
    nearest: Track | None = None
    markers: list[Marker] | None = None


def build(tracks: list[Track], home: tuple[float, float], map_radius_km: float, watch_radius_km: float,
          now: float, oblast_key: str | None, oblast_names: tuple[str, ...]) -> Picture:
    projection = geo.Projection(home, map_radius_km)
    keep_km = map_radius_km * KEEP_FACTOR
    picture = Picture(markers=[])
    candidates: list[tuple[float, Track, tuple[float, float]]] = []
    for track in tracks:
        if track.area_only:
            if (oblast_key and track.region_key == oblast_key) or (track.region and track.region in oblast_names):
                picture.area_only_in_region += 1
            continue
        position = predicted(track, now)
        distance = geo.distance_km(home, position)
        if not track.advisory and distance <= watch_radius_km:
            picture.threats_in_watch += 1
        if distance <= keep_km:
            candidates.append((distance, track, position))
    candidates.sort(key=lambda c: c[0])
    if candidates:
        picture.nearest_km, picture.nearest = candidates[0][0], candidates[0][1]
    for distance, track, position in candidates:
        x, y = projection.project(position)
        if not (-OFFSCREEN_MARGIN_PX <= x <= geo.SCREEN + OFFSCREEN_MARGIN_PX
                and -OFFSCREEN_MARGIN_PX <= y <= geo.SCREEN + OFFSCREEN_MARGIN_PX):
            continue
        trail = [p for p in track.trail if now - TRAIL_WINDOW_S <= p[2] <= now][-MAX_TRAIL:]
        uncertainty = 0
        if track.approximate and track.uncertainty_km > 0:
            uncertainty = min(MAX_UNCERTAINTY_PX, round(track.uncertainty_km * projection.px_per_km))
        picture.markers.append(Marker(
            x, y, track.heading, track.kind, track.approximate, track.stale, track.advisory, uncertainty,
            [projection.project((lat, lon)) for lat, lon, _ in trail]))
        if len(picture.markers) == MAX_MARKERS:
            break
    return picture

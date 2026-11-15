"""Sphere maths, the screen projection and point-in-polygon tests."""

from __future__ import annotations

import math

EARTH_RADIUS_KM = 6371.0
KM_PER_DEG = EARTH_RADIUS_KM * math.pi / 180
SCREEN = 240
HALF = SCREEN / 2
MAX_PX = 30000


def distance_km(a: tuple[float, float], b: tuple[float, float]) -> float:
    lat1, lat2 = math.radians(a[0]), math.radians(b[0])
    s = math.sin((lat2 - lat1) / 2)
    t = math.sin(math.radians(b[1] - a[1]) / 2)
    h = s * s + math.cos(lat1) * math.cos(lat2) * t * t
    return 2 * EARTH_RADIUS_KM * math.asin(math.sqrt(min(1.0, max(0.0, h))))


def destination(start: tuple[float, float], bearing_deg: float, km: float) -> tuple[float, float]:
    d = km / EARTH_RADIUS_KM
    b = math.radians(bearing_deg)
    lat1, lon1 = math.radians(start[0]), math.radians(start[1])
    lat2 = math.asin(max(-1.0, min(1.0, math.sin(lat1) * math.cos(d) + math.cos(lat1) * math.sin(d) * math.cos(b))))
    lon2 = lon1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(lat1), math.cos(d) - math.sin(lat1) * math.sin(lat2))
    return math.degrees(lat2), (math.degrees(lon2) + 540) % 360 - 180


class Projection:
    """Local equirectangular projection centred on home: `radius_km` maps to half the screen."""

    def __init__(self, home: tuple[float, float], radius_km: float):
        self.home = home
        self.px_per_km = HALF / max(radius_km, 0.1)
        self.cos_lat = math.cos(math.radians(home[0]))

    def project_float(self, point: tuple[float, float]) -> tuple[float, float]:
        dx = (point[1] - self.home[1]) * KM_PER_DEG * self.cos_lat
        dy = (point[0] - self.home[0]) * KM_PER_DEG
        return HALF + dx * self.px_per_km, HALF - dy * self.px_per_km

    def project(self, point: tuple[float, float]) -> tuple[int, int]:
        x, y = self.project_float(point)
        return (max(-MAX_PX, min(MAX_PX, round(x))), max(-MAX_PX, min(MAX_PX, round(y))))


def choose_rings(radius_km: float) -> list[int]:
    """Up to 3 ring radii: the largest step not above a third of the radius, and its multiples."""
    step = 1
    for candidate in (1, 2, 5, 10, 15, 20, 25, 50, 100):
        if candidate <= radius_km / 3:
            step = candidate
    rings = []
    for i in range(1, 4):
        km = step * i
        if km > radius_km + 0.01 or km > 255:
            break
        rings.append(km)
    return rings


def _in_ring(lon: float, lat: float, ring: list[list[float]]) -> bool:
    inside = False
    j = len(ring) - 1
    for i in range(len(ring)):
        xi, yi = ring[i][0], ring[i][1]
        xj, yj = ring[j][0], ring[j][1]
        if (yi > lat) != (yj > lat) and lon < (xj - xi) * (lat - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def polygons(geometry: dict) -> list[list[list[list[float]]]]:
    """GeoJSON Polygon or MultiPolygon as a list of polygons (outer ring, then holes)."""
    if geometry["type"] == "Polygon":
        return [geometry["coordinates"]]
    if geometry["type"] == "MultiPolygon":
        return geometry["coordinates"]
    return []


def contains(geometry: dict, lat: float, lon: float) -> bool:
    for polygon in polygons(geometry):
        if polygon and _in_ring(lon, lat, polygon[0]) and not any(_in_ring(lon, lat, h) for h in polygon[1:]):
            return True
    return False

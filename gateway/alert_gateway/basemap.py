"""The clock's 16-colour basemap: region fills and borders, water, roads and labels.

Layers are added in priority order onto one 240 x 240 index image. After each optional layer the
rows are run-length encoded, and the layer is kept only if the signed message still fits in
16,384 bytes. Fills, borders and the home label are required.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from . import geo, wire
from .config import ConfigError
from .wire import LEVEL_RED, LEVEL_YELLOW


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


GROUND, FILL_RED, FILL_YELLOW, WATER, ROAD, RAION_BORDER, OBLAST_BORDER, LABEL, HOME_LABEL = range(9)
PALETTE = [
    rgb565(33, 42, 60),    # ground
    rgb565(107, 36, 51),   # red fill
    rgb565(92, 78, 46),    # yellow fill
    rgb565(44, 79, 134),   # water
    rgb565(58, 69, 96),    # roads
    rgb565(50, 61, 85),    # raion borders
    rgb565(84, 98, 128),   # oblast borders
    rgb565(127, 155, 224), # labels
    rgb565(230, 234, 242), # home label
] + [0] * 7
FILL_INDEX = {LEVEL_YELLOW: FILL_YELLOW, LEVEL_RED: FILL_RED}
HOME_CLEARANCE_PX = 18


@dataclass(frozen=True)
class Basemap:
    version: int
    palette: list[int]
    rows: list[bytes]

    def message(self, nonce: int, key: bytes) -> bytes:
        return wire.encode_basemap(self.palette, self.rows, nonce, key)


def _encode(image: Image.Image) -> list[bytes]:
    pixels = image.load()
    return [wire.encode_row([pixels[x, y] for x in range(geo.SCREEN)]) for y in range(geo.SCREEN)]


def _fits(rows: list[bytes]) -> bool:
    return wire.basemap_size(rows) <= wire.MAX_BASEMAP_BYTES


def _points(projection: geo.Projection, ring: list) -> list[tuple[float, float]]:
    return [projection.project_float((p[1], p[0])) for p in ring]


def _draw_region_fill(image: Image.Image, projection: geo.Projection, geometry: dict, index: int) -> None:
    """Fills the region's area; its holes keep whatever is already drawn under them."""
    mask = Image.new("1", image.size, 0)
    draw = ImageDraw.Draw(mask)
    for polygon in geo.polygons(geometry):
        draw.polygon(_points(projection, polygon[0]), fill=1)
        for hole in polygon[1:]:
            draw.polygon(_points(projection, hole), fill=0)
    image.paste(index, mask=mask)


def _draw_borders(draw: ImageDraw.ImageDraw, projection: geo.Projection, oblasts: dict, raions: dict) -> None:
    for feature in raions["features"]:
        _draw_border(draw, projection, feature["geometry"], RAION_BORDER)
    for feature in oblasts["features"]:
        _draw_border(draw, projection, feature["geometry"], OBLAST_BORDER)


def _draw_border(draw: ImageDraw.ImageDraw, projection: geo.Projection, geometry: dict, index: int) -> None:
    for polygon in geo.polygons(geometry):
        for ring in polygon:
            points = _points(projection, ring)
            if len(points) > 1:
                draw.line(points + points[:1], fill=index, width=1)


def _way_points(projection: geo.Projection, element: dict) -> list[tuple[float, float]]:
    return [projection.project_float((p["lat"], p["lon"])) for p in element.get("geometry") or []
            if "lat" in p and "lon" in p]


def _label(draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont, text: str, x: float, y: float,
           index: int) -> None:
    width = draw.textlength(text, font=font)
    left = max(2, min(geo.SCREEN - 2 - width, x - width / 2))
    draw.text((left, y), text, fill=index, font=font, anchor="la")


def render(oblasts: dict, raions: dict, oblast_fills: dict[str, int], raion_fills: dict[str, int],
           osm: dict | None, home: tuple[float, float], radius_km: float, home_label: str,
           font_path: Path) -> Basemap:
    projection = geo.Projection(home, radius_km)
    font = ImageFont.truetype(str(font_path), 11)
    image = Image.new("L", (geo.SCREEN, geo.SCREEN), GROUND)
    draw = ImageDraw.Draw(image)
    draw.fontmode = "1"  # no anti-aliasing: every pixel must stay a palette index

    # Required: fills (yellow below red, so the more severe level shows) and borders.
    for level in (LEVEL_YELLOW, LEVEL_RED):
        for features, filled in ((oblasts, oblast_fills), (raions, raion_fills)):
            for feature in features["features"]:
                if filled.get(feature["properties"].get("key")) == level:
                    _draw_region_fill(image, projection, feature["geometry"], FILL_INDEX[level])
    _draw_borders(draw, projection, oblasts, raions)
    _label(draw, font, home_label, geo.HALF, geo.HALF + 8, HOME_LABEL)
    rows = _encode(image)
    if not _fits(rows):
        raise ConfigError("the region fills, borders and home label alone exceed 16 KB; reduce radius_km")

    elements = (osm or {}).get("elements", [])

    def water(d: ImageDraw.ImageDraw) -> None:
        for e in elements:
            tags = e.get("tags", {})
            if tags.get("natural") == "water":
                if e.get("type") == "way":
                    points = _way_points(projection, e)
                    if len(points) > 2:
                        d.polygon(points, fill=WATER)
                for member in e.get("members", []):  # relations: draw outer members as shores
                    if member.get("role") == "outer":
                        points = _way_points(projection, member)
                        if len(points) > 1:
                            d.line(points, fill=WATER, width=3)
            elif tags.get("waterway") == "river":
                points = _way_points(projection, e)
                if len(points) > 1:
                    d.line(points, fill=WATER, width=3)

    def roads(d: ImageDraw.ImageDraw) -> None:
        for e in elements:
            if "highway" in e.get("tags", {}):
                points = _way_points(projection, e)
                if len(points) > 1:
                    d.line(points, fill=ROAD, width=1)

    def places(d: ImageDraw.ImageDraw) -> None:
        for e in elements:
            tags = e.get("tags", {})
            if e.get("type") != "node" or "place" not in tags:
                continue
            name = tags.get("name:uk") or tags.get("name")
            if not name:
                continue
            x, y = projection.project_float((e["lat"], e["lon"]))
            if not (0 <= x < geo.SCREEN and 0 <= y < geo.SCREEN):
                continue
            if abs(x - geo.HALF) < 40 and abs(y - geo.HALF) < HOME_CLEARANCE_PX + 6:
                continue  # keep the home marker and label clear
            _label(d, font, name, x, y - 6, LABEL)

    for layer in (water, roads, places):
        candidate = image.copy()
        overlay = ImageDraw.Draw(candidate)
        overlay.fontmode = "1"
        layer(overlay)
        # Re-draw the required overlays so optional layers never hide borders or the home label.
        _draw_borders(overlay, projection, oblasts, raions)
        _label(overlay, font, home_label, geo.HALF, geo.HALF + 8, HOME_LABEL)
        candidate_rows = _encode(candidate)
        if _fits(candidate_rows):
            image, rows = candidate, candidate_rows
    return Basemap(wire.basemap_version(PALETTE, rows), PALETTE, rows)

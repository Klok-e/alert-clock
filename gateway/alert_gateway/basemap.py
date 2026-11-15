"""The clock's 16-colour basemap: region fills and borders, water, roads and labels.

Layers are added in priority order onto one 240 x 240 index image. After each optional layer the
rows are run-length encoded, and the layer is kept only if the signed message still fits in
20,480 bytes. Fills, borders and the home label are required.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

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
    rgb565(38, 58, 94),    # water
    rgb565(58, 69, 96),    # roads
    rgb565(50, 61, 85),    # raion borders
    rgb565(84, 98, 128),   # oblast borders
    rgb565(127, 155, 224), # labels
    rgb565(230, 234, 242), # home label
] + [0] * 7
FILL_INDEX = {LEVEL_YELLOW: FILL_YELLOW, LEVEL_RED: FILL_RED}
HOME_CLEARANCE_PX = 18
MIN_WATER_PX = 20.0  # smaller ponds are speckle on a 240-pixel screen


@dataclass(frozen=True)
class Basemap:
    version: int
    palette: list[int]
    rows: list[bytes]

    def message(self, nonce: int, key: bytes) -> bytes:
        return wire.encode_basemap(self.palette, self.rows, nonce, key)


def _encode(image: Image.Image, first_row: int = 0, end_row: int = geo.SCREEN) -> list[bytes]:
    pixels = image.load()
    return [wire.encode_row([pixels[x, y] for x in range(geo.SCREEN)]) for y in range(first_row, end_row)]


def _fits(rows: list[bytes]) -> bool:
    return wire.basemap_size(rows) <= wire.MAX_BASEMAP_BYTES


def _points(projection: geo.Projection, ring: list) -> list[tuple[float, float]]:
    return [projection.project_float((p[1], p[0])) for p in ring]


def _area_mask(size: tuple[int, int], outers: list[list[tuple[float, float]]],
               inners: list[list[tuple[float, float]]]) -> Image.Image:
    """1 inside the outer rings and 0 in their holes, so pasting through it keeps what the holes
    already show."""
    mask = Image.new("1", size, 0)
    draw = ImageDraw.Draw(mask)
    for ring in outers:
        draw.polygon(ring, fill=1)
    for ring in inners:
        draw.polygon(ring, fill=0)
    return mask


def _draw_region_fill(image: Image.Image, projection: geo.Projection, geometry: dict, index: int) -> None:
    for polygon in geo.polygons(geometry):
        holes = [_points(projection, hole) for hole in polygon[1:]]
        image.paste(index, mask=_area_mask(image.size, [_points(projection, polygon[0])], holes))


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


def _area_px(points: list[tuple[float, float]]) -> float:
    return abs(sum(points[i][0] * points[i - 1][1] - points[i - 1][0] * points[i][1]
                   for i in range(len(points)))) / 2


def _rings(members: list[dict], role: str) -> list[list[tuple[float, float]]]:
    """Joins a multipolygon's member ways of one role into closed rings of (lat, lon), end to end;
    a chain that does not close is left out."""
    open_ways = [[(p["lat"], p["lon"]) for p in m.get("geometry") or [] if "lat" in p and "lon" in p]
                 for m in members if m.get("type") == "way" and m.get("role") == role]
    open_ways = [w for w in open_ways if len(w) > 1]
    rings = []
    while open_ways:
        ring = open_ways.pop()
        while ring[0] != ring[-1]:
            for i, way in enumerate(open_ways):
                if way[0] == ring[-1]:
                    ring += way[1:]
                elif way[-1] == ring[-1]:
                    ring += way[-2::-1]
                else:
                    continue
                del open_ways[i]
                break
            else:
                break
        if ring[0] == ring[-1]:
            rings.append(ring)
    return rings


def _multipolygon_mask(size: tuple[int, int], projection: geo.Projection, members: list[dict]) -> Image.Image:
    def projected(role: str) -> list[list[tuple[float, float]]]:
        rings = [[projection.project_float(p) for p in ring] for ring in _rings(members, role)]
        return [ring for ring in rings if _area_px(ring) >= MIN_WATER_PX]
    return _area_mask(size, projected("outer"), projected("inner"))


@dataclass(frozen=True)
class LabelPlacement:
    origin: tuple[float, float]
    padded_bounds: tuple[float, float, float, float]


def _label_placement(draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont, text: str, x: float,
                     y: float) -> LabelPlacement:
    width = draw.textlength(text, font=font)
    left = max(2, min(geo.SCREEN - 2 - width, x - width / 2))
    _, top, _, bottom = font.getbbox(text, anchor="la")
    return LabelPlacement((left, y), (left - 2, y + top - 2, left + width + 2, y + bottom + 2))


def _label(draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont, text: str, placement: LabelPlacement,
           index: int) -> None:
    draw.text(placement.origin, text, fill=index, font=font, anchor="la")


def render(oblasts: dict, raions: dict, oblast_fills: dict[str, int], raion_fills: dict[str, int],
           osm: dict | None, home: tuple[float, float], radius_km: float, home_label: str,
           font_path: Path) -> Basemap:
    projection = geo.Projection(home, radius_km)
    font = ImageFont.truetype(str(font_path), 11)
    image = Image.new("L", (geo.SCREEN, geo.SCREEN), GROUND)
    draw = ImageDraw.Draw(image)
    draw.fontmode = "1"  # no anti-aliasing: every pixel must stay a palette index
    home_placement = _label_placement(draw, font, home_label, geo.HALF, geo.HALF + 8)

    # Required: fills (yellow below red, so the more severe level shows) and borders.
    for level in (LEVEL_YELLOW, LEVEL_RED):
        for features, filled in ((oblasts, oblast_fills), (raions, raion_fills)):
            for feature in features["features"]:
                if filled.get(feature["properties"].get("key")) == level:
                    _draw_region_fill(image, projection, feature["geometry"], FILL_INDEX[level])
    _draw_borders(draw, projection, oblasts, raions)
    _label(draw, font, home_label, home_placement, HOME_LABEL)
    rows = _encode(image)
    if not _fits(rows):
        raise ConfigError("the region fills, borders and home label alone exceed 20 KB; reduce radius_km")

    elements = (osm or {}).get("elements", [])

    def water(candidate: Image.Image) -> None:
        """Water paints only bare ground: alert fills stay visible over a reservoir."""
        mask = Image.new("1", candidate.size, 0)
        d = ImageDraw.Draw(mask)
        for e in elements:
            tags = e.get("tags", {})
            if tags.get("natural") == "water":
                if e.get("type") == "way":
                    points = _way_points(projection, e)
                    if len(points) > 2 and _area_px(points) >= MIN_WATER_PX:
                        d.polygon(points, fill=1)
                elif e.get("type") == "relation":
                    mask = ImageChops.logical_or(mask, _multipolygon_mask(mask.size, projection,
                                                                          e.get("members", [])))
                    d = ImageDraw.Draw(mask)
            elif tags.get("waterway") == "river" and tags.get("name"):
                points = _way_points(projection, e)
                if len(points) > 1:
                    d.line(points, fill=1, width=1)
        ground = candidate.point(lambda v: 255 if v == GROUND else 0).convert("1")
        candidate.paste(WATER, mask=ImageChops.logical_and(mask, ground))

    def roads(candidate: Image.Image) -> None:
        d = ImageDraw.Draw(candidate)
        for e in elements:
            if "highway" in e.get("tags", {}):
                points = _way_points(projection, e)
                if len(points) > 1:
                    d.line(points, fill=ROAD, width=1)

    def place_labels() -> list[tuple[str, float, float]]:
        """Town names on screen, most important first: cities, then by population, then nearest."""
        found = []
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
            population = "".join(ch for ch in tags.get("population", "") if ch.isdigit())
            rank = (tags["place"] != "city", -int(population) if population else 0,
                    (x - geo.HALF) ** 2 + (y - geo.HALF) ** 2)
            found.append((rank, name, x, y))
        return [(name, x, y) for _, name, x, y in sorted(found)]

    def keep_if_fits(layer, first_row: int = 0, last_row: int = geo.SCREEN - 1) -> bool:
        """Adds a layer that changes only rows first_row..last_row, if the result still fits."""
        nonlocal image, rows
        candidate = image.copy()
        layer(candidate)
        # Re-draw the required overlays so optional layers never hide borders or the home label.
        overlay = ImageDraw.Draw(candidate)
        overlay.fontmode = "1"
        _draw_borders(overlay, projection, oblasts, raions)
        _label(overlay, font, home_label, home_placement, HOME_LABEL)
        first, last = max(0, first_row), min(geo.SCREEN - 1, last_row)
        candidate_rows = rows[:first] + _encode(candidate, first, last + 1) + rows[last + 1:]
        if not _fits(candidate_rows):
            return False
        image, rows = candidate, candidate_rows
        return True

    def text_layer(name: str, placement: LabelPlacement):
        def draw_text(candidate: Image.Image) -> None:
            d = ImageDraw.Draw(candidate)
            d.fontmode = "1"
            _label(d, font, name, placement, LABEL)
        return draw_text

    for layer in (water, roads):
        keep_if_fits(layer)
    # Labels one at a time, each only if it clears the ones placed and still fits; a label
    # changes only the rows of its box, so only those are encoded again.
    placed: list[tuple[float, float, float, float]] = []
    for name, x, y in place_labels():
        placement = _label_placement(draw, font, name, x, y - 6)
        box = placement.padded_bounds
        if any(box[0] < b[2] and b[0] < box[2] and box[1] < b[3] and b[1] < box[3] for b in placed):
            continue
        if keep_if_fits(text_layer(name, placement), int(box[1]), int(box[3]) + 1):
            placed.append(box)
    return Basemap(wire.basemap_version(PALETTE, rows), PALETTE, rows)

"""Encoding of the clock protocol (gateway/PROTOCOL.md), plus decoders for tests."""

from __future__ import annotations

import hashlib
import hmac
import struct
import zlib
from dataclasses import dataclass, field

VERSION = 3
TYPE_SNAPSHOT = 1
TYPE_BASEMAP = 2
MAC_BYTES = 32
MAX_MARKERS = 24
MAX_TRAIL = 6
MAX_RINGS = 3
MAX_BASEMAP_BYTES = 20480
SCREEN = 240
NO_DISTANCE = 0xFFFF
NO_DURATION = 0xFFFFFFFF

# core::AlertLevel
LEVEL_UNKNOWN, LEVEL_NONE, LEVEL_PARTIAL, LEVEL_YELLOW, LEVEL_RED = range(5)

# core::ThreatKind
KINDS = ["uav", "recon", "fpv", "missile", "ballistic", "kab", "aircraft", "unknown"]
KIND_UNKNOWN = 7


@dataclass
class Marker:
    x: int
    y: int
    heading: int = -1
    kind: int = KIND_UNKNOWN
    approximate: bool = False
    stale: bool = False
    advisory: bool = False
    uncertainty_px: int = 0
    trail: list[tuple[int, int]] = field(default_factory=list)


@dataclass
class Snapshot:
    alert_level: int = LEVEL_UNKNOWN
    alerts_stale: bool = True
    threats_stale: bool = True
    alerts_age_s: int = 0xFFFF
    threats_age_s: int = 0xFFFF
    alert_duration_s: int = NO_DURATION  # since home's alert started
    basemap_version: int = 0
    px_per_km: float = 4.0
    rings_km: list[int] = field(default_factory=list)
    threats_in_watch: int = 0
    area_only_in_region: int = 0
    nearest_km: float | None = None
    nearest_kind: int = KIND_UNKNOWN
    nearest_heading: int = -1
    nearest_advisory: bool = False
    nearest_stale: bool = False
    markers: list[Marker] = field(default_factory=list)


def sign(key: bytes, body: bytes) -> bytes:
    return body + hmac.new(key, body, hashlib.sha256).digest()


def verify(key: bytes, message: bytes) -> bytes:
    """Returns the body without its MAC; raises ValueError when the MAC is wrong."""
    if len(message) < MAC_BYTES:
        raise ValueError("too short")
    body, mac = message[:-MAC_BYTES], message[-MAC_BYTES:]
    if not hmac.compare_digest(hmac.new(key, body, hashlib.sha256).digest(), mac):
        raise ValueError("bad MAC")
    return body


def _clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def _i16(value: int) -> int:
    return _clamp(int(value), -32768, 32767)


def encode_snapshot(snapshot: Snapshot, nonce: int, key: bytes) -> bytes:
    out = bytearray(struct.pack("<BBI", VERSION, TYPE_SNAPSHOT, nonce & 0xFFFFFFFF))
    flags = (1 if snapshot.alerts_stale else 0) | (2 if snapshot.threats_stale else 0)
    rings = list(snapshot.rings_km[:MAX_RINGS])
    nearest = NO_DISTANCE if snapshot.nearest_km is None else _clamp(round(snapshot.nearest_km * 10), 0, 0xFFFE)
    out += struct.pack(
        "<BBHHIIHB3BHHHBhBB",
        snapshot.alert_level, flags,
        _clamp(snapshot.alerts_age_s, 0, 0xFFFF), _clamp(snapshot.threats_age_s, 0, 0xFFFF),
        _clamp(snapshot.alert_duration_s, 0, NO_DURATION),
        snapshot.basemap_version & 0xFFFFFFFF,
        _clamp(round(snapshot.px_per_km * 100), 0, 0xFFFF),
        len(rings), *(rings + [0] * (MAX_RINGS - len(rings))),
        _clamp(snapshot.threats_in_watch, 0, 0xFFFF), _clamp(snapshot.area_only_in_region, 0, 0xFFFF),
        nearest, snapshot.nearest_kind, snapshot.nearest_heading,
        (1 if snapshot.nearest_advisory else 0) | (2 if snapshot.nearest_stale else 0),
        min(len(snapshot.markers), MAX_MARKERS),
    )
    for m in snapshot.markers[:MAX_MARKERS]:
        trail = m.trail[-MAX_TRAIL:]
        flags = (1 if m.approximate else 0) | (2 if m.stale else 0) | (4 if m.advisory else 0)
        out += struct.pack("<hhhBBBB", _i16(m.x), _i16(m.y), m.heading, m.kind, flags,
                           _clamp(m.uncertainty_px, 0, 255), len(trail))
        for x, y in trail:
            out += struct.pack("<hh", _i16(x), _i16(y))
    return sign(key, bytes(out))


def basemap_version(palette: list[int], rows: list[bytes]) -> int:
    crc = zlib.crc32(struct.pack("<16H", *palette))
    for row in rows:
        crc = zlib.crc32(row, crc)
    return crc or 1  # 0 means "no basemap"


def basemap_size(rows: list[bytes]) -> int:
    """Signed message size for these rows."""
    return 6 + 4 + 32 + SCREEN * 2 + 2 + sum(len(r) for r in rows) + MAC_BYTES


def encode_basemap(palette: list[int], rows: list[bytes], nonce: int, key: bytes) -> bytes:
    if len(palette) != 16 or len(rows) != SCREEN:
        raise ValueError("basemap needs 16 colours and 240 rows")
    offsets = []
    position = 0
    for row in rows:
        offsets.append(position)
        position += len(row)
    out = bytearray(struct.pack("<BBI", VERSION, TYPE_BASEMAP, nonce & 0xFFFFFFFF))
    out += struct.pack("<I", basemap_version(palette, rows))
    out += struct.pack("<16H", *palette)
    out += struct.pack(f"<{SCREEN}H", *offsets)
    out += struct.pack("<H", position)
    out += b"".join(rows)
    message = sign(key, bytes(out))
    if len(message) > MAX_BASEMAP_BYTES:
        raise ValueError(f"basemap is {len(message)} bytes, over {MAX_BASEMAP_BYTES}")
    return message


def encode_row(indices: list[int]) -> bytes:
    """Run-length encodes one row of 240 palette indices: a run count, then one byte per run,
    length << 4 | index, for runs of 1-15 pixels, or the index alone (length nibble 0) followed by
    a length byte for longer runs."""
    runs: list[tuple[int, int]] = []
    for value in indices:
        if runs and runs[-1][1] == value and runs[-1][0] < 255:
            runs[-1] = (runs[-1][0] + 1, value)
        else:
            runs.append((1, value))
    if len(runs) > 255:
        raise ValueError("row has more than 255 runs")
    out = bytearray([len(runs)])
    for length, value in runs:
        out += bytes([length << 4 | value & 0x0F]) if length <= 15 else bytes([value & 0x0F, length])
    return bytes(out)


def decode_row(row: bytes) -> list[int]:
    out: list[int] = []
    position = 1
    for _ in range(row[0]):
        tag = row[position]
        length = tag >> 4
        position += 1
        if length == 0:
            length = row[position]
            position += 1
        out += [tag & 0x0F] * length
    return out


def decode_snapshot(message: bytes, key: bytes, nonce: int) -> Snapshot:
    body = verify(key, message)
    version, kind, echoed = struct.unpack_from("<BBI", body, 0)
    if version != VERSION or kind != TYPE_SNAPSHOT or echoed != nonce:
        raise ValueError("wrong version, type or nonce")
    fields = struct.unpack_from("<BBHHIIHB3BHHHBhBB", body, 6)
    (level, flags, alerts_age, threats_age, duration, version_map, scale, ring_count, r0, r1, r2,
     in_watch, area_only, nearest, nearest_kind, nearest_heading, nearest_flags, count) = fields
    s = Snapshot(level, bool(flags & 1), bool(flags & 2), alerts_age, threats_age, duration, version_map,
                 scale / 100, [r0, r1, r2][:ring_count], in_watch, area_only,
                 None if nearest == NO_DISTANCE else nearest / 10, nearest_kind, nearest_heading,
                 bool(nearest_flags & 1), bool(nearest_flags & 2))
    position = 6 + struct.calcsize("<BBHHIIHB3BHHHBhBB")
    for _ in range(count):
        x, y, heading, k, f, unc, trail_count = struct.unpack_from("<hhhBBBB", body, position)
        position += 10
        trail = [struct.unpack_from("<hh", body, position + 4 * i) for i in range(trail_count)]
        position += 4 * trail_count
        s.markers.append(Marker(x, y, heading, k, bool(f & 1), bool(f & 2), bool(f & 4), unc, trail))
    if position != len(body):
        raise ValueError("length mismatch")
    return s


def decode_basemap(message: bytes, key: bytes, nonce: int) -> tuple[int, list[int], list[list[int]]]:
    body = verify(key, message)
    version, kind, echoed = struct.unpack_from("<BBI", body, 0)
    if version != VERSION or kind != TYPE_BASEMAP or echoed != nonce:
        raise ValueError("wrong version, type or nonce")
    (map_version,) = struct.unpack_from("<I", body, 6)
    palette = list(struct.unpack_from("<16H", body, 10))
    offsets = struct.unpack_from(f"<{SCREEN}H", body, 42)
    (rows_length,) = struct.unpack_from("<H", body, 42 + 2 * SCREEN)
    rows_area = body[44 + 2 * SCREEN:]
    if len(rows_area) != rows_length:
        raise ValueError("length mismatch")
    rows = [decode_row(rows_area[o:]) for o in offsets]
    if any(len(r) != SCREEN for r in rows):
        raise ValueError("row length")
    return map_version, palette, rows

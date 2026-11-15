# Clock ↔ gateway protocol, version 1

Plain HTTP/1.0 on the gateway's port (default 8080). Every value is little-endian. Every reply
body ends with a 32-byte HMAC-SHA256 over all bytes before it, keyed with the shared 32-byte
key (`hmac_key` in `/etc/alert-gateway.toml`, `CLOCK_GATEWAY_KEY` in the clock's
`include/secrets.h`, both as 64 hex characters).

The clock rejects a body whose MAC, version, type or nonce does not match, or whose length
disagrees with its own counts. Python: `gateway/alert_gateway/wire.py`; C++:
`lib/core/src/wire.{h,cpp}`. `tools/make_wire_fixtures.py` writes sample bodies (key
`00 01 02 … 1f`) from the Python encoder to `test/wire_fixtures.h` for the C++ tests; after a
change to either side, rerun it (a pytest fails until you do).

## Common header (6 bytes)

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | version = 1 |
| 1 | u8 | type: 1 snapshot, 2 basemap |
| 2 | u32 | nonce, echoed from the request's `n` parameter (8 hex digits) |

## `GET /v1/snapshot?n=<nonce>` → type 1

| Type | Field |
| --- | --- |
| u8 | alert level: 0 unknown, 1 none, 2 partial, 3 yellow, 4 red (`core::AlertLevel`) |
| u8 | flags: bit 0 alerts stale, bit 1 threats stale |
| u16 | alerts age, s: since the alerts source's last good update, saturating at 65,535 |
| u16 | threats age, s: the same for threats |
| u32 | basemap version (CRC-32 of palette and rows); 0 when there is no basemap |
| u16 | map scale, px per km × 100 |
| u8 | ring count (0–3), then 3 × u8 ring radii in km (unused ones 0) |
| u16 | `threats_in_watch` |
| u16 | `area_only_in_region` |
| u16 | nearest track distance in 0.1 km; 65,535 when there is none |
| u8 | nearest track kind |
| i16 | nearest track heading, degrees 0–359, −1 unknown |
| u8 | nearest track flags: bit 0 advisory, bit 1 stale |
| u8 | marker count (0–24), then the markers |

Marker: i16 x, i16 y (screen pixels, home at 120, 120), i16 heading (−1 unknown), u8 kind,
u8 flags (bit 0 approximate, bit 1 stale, bit 2 advisory), u8 uncertainty radius in px,
u8 trail count (0–6), then trail count × (i16 x, i16 y), oldest first.

Kinds follow `core::ThreatKind`: 0 uav, 1 recon, 2 fpv, 3 missile, 4 ballistic, 5 kab,
6 aircraft, 7 unknown.

A snapshot with 24 markers of 6 trail points is 881 bytes (6 header + 27 fixed + 24 × 34 + 32 MAC); the clock's buffer is 1,024.

## `GET /v1/basemap?n=<nonce>` → type 2

| Type | Field |
| --- | --- |
| u32 | basemap version |
| 16 × u16 | palette, RGB565 |
| 240 × u16 | row offsets into the rows area |
| u16 | rows area length |
| bytes | rows area |

Each row: u8 run count, then run count × (u8 length 1–255, u8 palette index 0–15); a row's
lengths sum to 240. The whole body, MAC included, is at most 16,384 bytes. When the gateway has
no basemap it answers 404.

## Test endpoint (`--test` only)

`GET /_test/time?unix=<s>` sets the gateway's clock, loads the region files and the Overpass
layers if it has none yet, refreshes both NEPTUN sources over REST at once, and answers 200
after the refresh. It does not exist without `--test`.

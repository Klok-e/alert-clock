# Gateway design

Status: built (2026-11-15); verified on the PC against the mock server, not yet on a Pi or
the clock. `gateway/PROTOCOL.md` holds the byte layout.

A Raspberry Pi on the clock's LAN fetches every upstream source, computes what the screen needs,
and serves it to the clock as a small signed binary snapshot. The clock keeps wall-clock time
(NTP), turns the snapshot into a panel and banner with `plan_screen`, and draws.

```
NEPTUN (WebSocket + REST, region GeoJSON) ─┐
Overpass (once per config)                 ┴─► Pi gateway ─► HTTP every 2 s, signed ─► clock
```

## Ownership

| Concern | Owner |
| --- | --- |
| Home location, map radius, watch radius | Pi config only |
| Alert level (`AlertLevel`: Unknown/None/Partial/Yellow/Red) and stale flags | Pi |
| Projection, dead reckoning, counts, nearest track | Pi |
| Banner and panel (`plan_screen`, `Banner`), the 60 s no-reply rule, sounds | Clock |
| Wall-clock time and date | Clock (NTP) |

## Upstream sources (Pi)

NEPTUN is the only alert and track source; alerts.in.ua and its token are gone. NEPTUN's rules
are at https://neptun.in.ua/developers and https://neptun.in.ua/api-terms; re-read both before
changing anything in this section, since both change. The developers page asks clients to poll
REST at most once every 5 s and recommends the WebSocket stream for live use.

- **Stream.** The Pi follows `wss://neptun.in.ua/api/v1/stream` for threats and alerts. NEPTUN
  sends a `heartbeat` frame about every 14 s; 60 s without any frame, or a closed socket,
  triggers a reconnect.
  Reconnect delays start at 5 s and double to a 5 min cap; a stable hour resets them.
- **REST.** Every HTTP request to neptun.in.ua (REST endpoints and region files) goes through
  one queue that starts requests at least 5 s apart, so NEPTUN never sees more than one request
  per 5 s, whatever the reconnect pattern. On every (re)connect, and every 5 min while connected,
  the Pi queues `/api/v1/threats` then `/api/v1/alerts`, unless they are already queued. While
  the stream is down, it queues the same pair every 30 s, so a stream outage alone never makes a
  source stale.
- **Stream frames** (`{type, data}`): `alerts` (`data`: `raions` and `oblasts`, the same
  arrays as REST) replaces the alert state; `snapshot` (`data.threats`) replaces the track
  state; `upsert` (`data`: one track) and `remove` (`data.id`) change one track by `id`; each
  5 min REST fetch replaces the state. Malformed frames are logged and ignored.
- **Last good update**, per source (threats, alerts): the latest of a successful REST fetch of
  that source, a stream frame for that source, or a heartbeat on the current connection while
  that source's most recent REST fetch on this connection succeeded. Heartbeats keep a quiet
  source fresh between REST refreshes; a failed REST refresh stops them counting for that source until a
  REST fetch or a frame for it succeeds, so a connection that has stopped sending a source goes
  stale within 180 s of the next refresh failing.
- **Stale flags.** Built into every reply as "last good update more than 180 s ago", computed per
  reply and never cached.
- **Region files.** `/oblasts.geojson` and `/raions.geojson` are fetched at start-up and cached on
  disk; the cache is refreshed once a day, and a failed or unparseable refresh keeps the
  previous copy. The gateway serves from start-up; until the home region is resolved, the alerts
  source counts as stale, `area_only_in_region` is 0 and nothing is filled, while tracks and
  counts work from the coordinates alone. With no cache and NEPTUN unreachable, the alerts
  source stays stale (the clock shows NoData) and the Pi retries on the reconnect schedule.
- **Home region.** At start-up the Pi tests the configured home coordinates against both files
  and logs the raion and oblast it found. Kyiv city has no raions: it resolves to oblast key
  `м. київ` and no raion. Coordinates that fall in no oblast are a configuration error: the
  service logs it and exits, and systemd shows it failed.
- **Alert level** from `/api/v1/alerts`, whose `raions` and `oblasts` arrays hold the active
  entries (`key`, `name`, `oblast`, `since`, `level`):
  - an entry's level is Yellow when `level` is `yellow`, and Red otherwise (including `red`,
    missing or unknown values);
  - only the smallest unit home lies in gives Yellow or Red: the home raion's entry (in `raions`,
    matched by `key`), or for a home without a raion (Kyiv city) the home oblast's entry (in
    `oblasts`, matched by `key`). An oblast entry does not alert a home with a raion: an oblast
    alert can exclude raions, and the live feed lists such alerts as the alerted raions. A home
    outside Kyiv city that lies in no raion gets at most Partial, and start-up logs a warning;
  - otherwise, the home oblast's entry, or any `raions` entry whose `oblast` equals the home
    oblast feature's `region` property, gives Partial. A Kyiv city home never gets Partial:
    Kyiv city has no raions, and alerts in Київська область do not count;
  - otherwise None. A stale alerts source sends Unknown.
- **Alert duration.** With a Yellow or Red level, the time since the earliest readable `since`
  among the entries that give home's level, so an escalation from yellow to red keeps counting.
  None for Partial, None and Unknown, or when no home entry has a readable `since`.
- **Geometry.** Home is the screen centre (120, 120); `px_per_km` is 120 / map radius. Ring
  radii follow `choose_rings` in `gateway/alert_gateway/geo.py`: the largest step from
  1, 2, 5, 10, 15, 20, 25, 50, 100 km not above a third of the map radius, and up to 3 of its
  multiples within the map radius.
- **Tracks** from `/api/v1/threats`:
  - tracks with status `resolved` are ignored everywhere;
  - each track's position is dead-reckoned to the reply time from its observed time along its
    velocity bearing and speed, bounded by NEPTUN’s per-type time and distance caps;
    distances are from home to that position, and
    counts, nearest and markers all use it. Prediction requires an active track, a readable
    `confirmedAt`, and velocity bearing and speed; otherwise the reported coordinates stay fixed;
  - counts cover every other track in the feed: `threats_in_watch` is non-advisory,
    non-`areaOnly` tracks within the watch radius; `area_only_in_region` is `areaOnly` tracks
    whose `regionKey` equals the home oblast's key, or whose `region` equals the home oblast
    feature's `region` property (`name` for `м. київ`); the nearest-track fields describe the
    nearest non-`areaOnly` track within 1.5 × the map radius (advisory or not, with its
    advisory flag);
  - markers: non-`areaOnly` tracks are projected to signed 16-bit screen pixels; those more than
    24 px outside the screen are dropped; the 24 nearest of the rest are sent. Each carries
    up to 6 points sampled evenly by point order from NEPTUN's `trail` field within
    15 min before the reply time, preserving the oldest and newest retained points,
    also projected, and an uncertainty radius (approximate tracks only, `uncertaintyKm` ×
    `px_per_km`, capped at 200 px). The clock clips what it draws;
  - a marker's `stale` flag is NEPTUN's per-track status `stale` (drawn faded); it is unrelated
    to the per-source stale flags;
  - `areaOnly` tracks are never drawn, given a distance or a heading. Advisory tracks are drawn
    grey and hollow and never cause a sound (NEPTUN's developers page: no red, no siren).

## Wire protocol (clock ↔ Pi)

The byte layout is in `gateway/PROTOCOL.md`. `tools/make_wire_fixtures.py` encodes sample
messages with the Python encoder into `test/wire_fixtures.h`; the C++ tests decode them, and a
pytest fails while that file is out of date with the encoder.

- The gateway listens on `0.0.0.0:8080`. The clock sends `GET /v1/snapshot?n=<nonce>` every 2 s;
  the nonce is a fresh 32-bit value from the ESP8266 hardware RNG (`RANDOM_REG32`).
- Replies are little-endian, fixed layout, starting with a version byte, and end with an
  HMAC-SHA256 over everything before it, which includes the echoed nonce. The clock rejects a
  reply with a bad MAC, a different nonce or an unknown version, and checks the MAC with its
  own SHA-256 in `lib/core/src/sha256.cpp`, which needs no heap.
- A snapshot carries: the alert level; both stale flags and both sources' ages; the alert
  duration; the
  basemap version; the map scale (`px_per_km`) and up to 3 ring radii in km; the counts and
  nearest-track fields (distance, kind, heading, advisory, stale); and up to 24 markers, each
  with x, y, heading, kind, flags (approximate, stale, advisory), uncertainty radius in pixels,
  a reported-position anchor, a prediction flag, and up to 6 trail points. A whole reply is at
  most 981 bytes and fits the clock's 1 KB buffer.
- `GET /v1/basemap?n=<nonce>` returns the basemap message (header with version and palette, row
  table, rows, MAC), at most 20,480 bytes in all. The basemap version is the CRC-32 of the
  encoded rows and palette, so any content change, including a Pi restart or a late Overpass
  layer, changes it. The clock fetches it at start-up and whenever a snapshot's version differs
  from the one it holds. It downloads straight into its one basemap buffer and marks the map
  invalid first; it draws the map without a basemap (rings only) until the whole message has
  arrived and its MAC and nonce check out. A failed or rejected download leaves it invalid, and
  the next snapshot triggers another try.
- The endpoints are readable by anything on the LAN: the data is public, and the MAC protects
  integrity only. NEPTUN's terms do not address serving its data to devices on a LAN; the clock
  meets its attribution and disclaimer rules on screen.

## Basemap

- 240 × 240 pixels, 4 bits per pixel against a 16-colour palette sent with it. Palette roles:
  ground, red fill, yellow fill, water, roads, borders, labels; the rest are spare.
- **Fills.** Each raion takes the more severe of its own entry and its oblast's entry; an oblast
  entry fills all of that oblast's raions; Kyiv city is one polygon filled from its oblast
  entry. Partial has no fill of its own: only the alerted raions are filled. While the alerts
  source is stale, nothing is filled.
- **Sources.** Borders and fills come from NEPTUN's region files. Water, roads and place names
  come from one Overpass query, cached on disk under a hash of the config and repeated only
  when the config changes; with no cache and Overpass unreachable, the basemap goes out without
  those layers while the Pi retries with backoff.
- **Redraw.** When any visible raion's fill changes, the Pi redraws the basemap (so its CRC-32
  version changes).
- **Encoding.** Each row is a run count followed by its runs, one byte (length and colour
  nibbles) for runs up to 15 pixels and two for longer ones, about 40% smaller than byte pairs
  on real maps of Kyiv and Kharkiv; a table of 240 16-bit row offsets lets the clock decode one
  4-row strip at a time. The whole basemap message fits in 20,480 bytes.
- **Water.** Water areas (`natural=water` ways, and relations whose member ways join into closed
  outer rings, with their inner rings as holes) are filled; areas under 20 screen pixels are left
  out as speckle. Named `waterway=river` ways are 1-pixel lines; unnamed ones are left out. Water
  paints only bare ground, so an alerted area keeps its fill over a reservoir, and its colour stays
  close to the ground so fills and tracks stand out.
- **Fitting, one pass, no retries.** Layers go in this order: fills and borders, home label,
  water, roads, then place labels one at a time: cities, then by population, then nearest home,
  skipping a label that would overlap one already placed. The Pi encodes each layer or label onto
  the result so far and keeps it only if the total still fits; one that does not fit is skipped
  and the next one is tried. Fills, borders and the home label are required: if they alone do not
  fit, the configuration (map radius too large for the area's borders) is an error, and the
  service logs it and exits. `gateway/tests/test_basemap.py` checks a real Overpass response
  around Kyiv (`fixtures/osm`) keeps roads and labels within the budget.
- **Text.** The Pi renders labels with Pillow in Inter (SIL Open Font Licence,
  https://github.com/rsms/inter), in Ukrainian.

## Clock firmware

- **Removed:** TLS and the root certificates, the NEPTUN and alerts.in.ua parsers, `geo`,
  ArduinoJson and its parse arena, the alerts token, the compile-time home and radius settings.
  Their logic and tests move to Python.
- **Added, as static buffers:** a 1,024-byte reply buffer and a 20,480-byte basemap buffer that
  holds the whole basemap message; plus the gateway source (HTTP every 2 s, nonce, MAC check) and a
  per-strip RLE decoder.
- **Freshness.** With no valid reply for 60 s, the clock treats both sources as stale: NoData
  banner, map data shown as old. Otherwise the Pi's stale flags decide.
- **Fetching.** DNS, connection, writes and reads advance without waiting for the network.
  A transaction has a 2 s DNS/connection deadline, a 5 s no-progress timeout and a 10 s total
  deadline. Each tick consumes at most 256 response bytes. Snapshot and basemap buffers stay
  fixed-capacity; the request buffer becomes header-line storage after transmission.
- **Retries.** A failed poll is retried after 10 s instead of 2 s, and a failed basemap download
  after 30 s. A long press or `POST /debug/refresh` forces a snapshot; while a transaction is
  active, refresh requests coalesce. Changing the gateway cancels the old transaction.
- **Gateway address.** Entered on the `/wifi` page as `host` or `host:port` (default port 8080)
  and saved in flash.
- **Inputs.** A long press and `POST /debug/refresh` poll the gateway at once.
  `POST /debug/sources?gateway=host[:port]` points the clock at another gateway until reboot.
  `/debug/state` reports heap, the gateway source's last outcome, successes, failures, MAC or
  nonce rejections and seconds since the last valid reply, and the basemap version and
  validity, and the alert duration shown.
- **Basemap diagnostics.** `/debug/state` keeps snapshot health under `gateway` and reports
  basemap transfer health separately under `basemap`: outcome, HTTP status, wire rejection,
  read termination (`none`, `closed`, `timeout` or `error`), expected and received body bytes,
  total duration in milliseconds, and success/failure counts. `expected_bytes` is −1 until a
  content length is known. `last_failure` preserves the latest failed transfer across a
  successful retry. A failed map download leaves the map invalid until the next successful
  retry; the retry interval remains 30 seconds.
- **Output profiles.** The clock owns a global sound switch, its day/night brightness, buzzer
  volume, three sound toggles per profile and an editable daily Kyiv-time schedule. Volume
  1–100% sets the buzzer's duty cycle (asin(v)/π, so the fundamental's amplitude follows v;
  100% is the 50% duty `tone()` used); sound off mutes automatic sounds, not previews.
  Defaults are sound on, day screen 80%, LED 100%, all sounds, volume 100%; night
  23:00–10:00, screen/LED 5%, volume 100%, automatic sounds off. Until time sync it uses
  night settings. Automatic sounds disabled by a profile change stop rather than resume later.
- **LED and sound playback.** One portable controller advances patterns from elapsed time
  while fetching continues. Only Yellow and Red alert: Partial keeps the LED off and plays no
  raised sound, and Yellow or Red falling to Partial counts as a clear. Alert start/escalation
  gives three gentle LED pulses; clear fades to off; NoData double-flashes amber every 5 s. The
  existing raised/escalation sound patterns remain, and clear is one 80 ms, 1,400 Hz chirp.
  Hardware writes are cached by value. Patterns also advance between the strips of a screen
  redraw (about 100 ms), so drawing does not stall them.
- **Settings and previews.** Authenticated `GET /settings` serves the page,
  `GET /settings/state` reads settings/status, `POST /settings` saves, and
  `POST /settings/preview` auditions submitted draft values without saving. A new preview
  replaces a running one but not a live sound. An LED preview fades over 300 ms from the shown
  colour to its pattern's start colour, holds it until 1 s, plays 2.5 s and fades back; other
  previews are capped at 3 s. A preview must finish before a pending real sound plays. The
  latest real event replaces the pending event; muted or obsolete events are discarded, with
  profile and alert relevance checked again on completion. Manual sound previews bypass
  scheduled muting.
  Save commits before applying settings; an active preview keeps its captured values.
  Safe mode rejects previews.
- **Storage.** The emulated EEPROM allocation is 89 bytes: gateway `GWY1` bytes 0–67 are
  preserved, and an explicitly encoded 21-byte versioned/CRC-checked record (version 2) stores
  output profiles, volumes, the sound switch and schedule. A version 1 record (18 bytes, no
  volumes) still loads, with sound on and full volume. Missing or invalid output records use defaults without writing.
  Failed commits leave live settings unchanged and restore staged bytes so a later unrelated
  save cannot commit a rejected change.
- **Threat symbols.** The map and nearest-track card distinguish UAVs (delta wing), FPV
  drones (four rotors), reconnaissance (eye), missiles (rocket), ballistic threats (bolt),
  guided bombs (bomb), aircraft (plane), and unknown types (question mark). The glyph stays
  upright; a larger outer pointer with light and dark outlines shows bearing and is omitted
  when heading is unknown. Four contrasting corner brackets frame the nearest track whose
  centre lies inside the map viewport; the brackets follow the distance ordering supplied
  by the gateway and remain distinct from uncertainty circles.
  Advisories remain hollow grey and stale symbols fade. Generic card states use a dot.
- **Paths.** Reported history is a bright two-pixel solid line with a dark outline, fading
  by point order toward its oldest end. The estimated segment from reported position to
  predicted marker is dashed. Uncertainty circles are subdued; advisories stay grey.
- **Alert pill.** A Yellow or Red pill adds the alert duration (the gateway's value plus the time
  since the last valid reply) in its two largest units: "Загроза · 23 хв", "Тривога · 1 год 23 хв",
  "Тривога · 2 д 3 год", "<1 хв" under a minute; "Повітряна тривога" shortens to "Тривога". Where
  that does not fit beside the map's time, the pill keeps the duration alone, in its colour.
- **Screens.** All text is Ukrainian, in Inter converted by `tools/make_fonts.py` (Pillow) to
  GFX fonts in `lib/ui/src/fonts.cpp`, holding ASCII and the Ukrainian Cyrillic letters (А–я,
  Ґґ, Єє, Іі, Її), plus `·` (U+00B7), `©` (U+00A9) and the apostrophe `ʼ` (U+02BC), kept in
  flash. The credit "дані: neptun.in.ua · © OpenStreetMap" is centred on one line.
  On the map the legend or data age occupies the footer line above the credit.
  At start-up the clock shows "Неофіційне джерело. Орієнтуйтеся на
  офіційні сигнали тривоги." for 4 s. NEPTUN's terms §3 (a visible link next to its data) and §5
  (state that it is unofficial and refer users to official signals) require these.
- **RAM.** The last build with HTTPS used 42,092 bytes of static RAM. Removing HTTPS-era state
  saves about 4 KB of static RAM and the about 25 KB HTTPS fetch peak; the new static buffers
  add about 18 KB. Static RAM came to about 55 KB, and the clock showed about 21 KB of free heap
  while serving `/debug/state` on Wi-Fi. The basemap buffer then grew from 16 KB to 20 KB, leaving
  about 17 KB of that heap, above the 12,000 bytes Wi-Fi and the web server need.

## Gateway on the Pi

- Python, run by systemd as the account that ran `sudo gateway/install.sh` (e.g. `pi`). The
  installer creates a virtualenv, copies
  `gateway/alert-gateway.example.toml` to `/etc/alert-gateway.toml` if it is absent, generates
  a random 32-byte `hmac_key` there as 64 hex characters, prints the matching `#define CLOCK_GATEWAY_KEY` line for
  the clock's `include/secrets.h`, and enables the `alert-gateway` service. Updates: `git pull`
  and a restart.
- `/etc/alert-gateway.toml` holds every setting: home coordinates, map and watch radii, HMAC
  key, listen port. Edit it and run `systemctl restart alert-gateway`; there is no settings page.

## Testing

The mock server becomes a fake NEPTUN (WebSocket and
REST, alert phases for home raion yellow and red, home oblast yellow (Partial), partial, missing `level` and Kyiv
city, plus small synthetic region files) and a fake Overpass. Scenarios run the whole path: fake
upstream → the real gateway → the native clock build → PNG frames. `tools/run_scenarios.py`
writes a gateway config from the scenario's `home` and radii, starts the gateway on a free port
with `--test`, and sets its time at each step through `GET /_test/time?unix=<s>`, an endpoint
that exists only under `--test`; it also makes the gateway fetch NEPTUN's REST endpoints and,
the first time, Overpass at once, so frames do not depend on timing. The gateway reads time
only through that injectable clock. Unit tests on both sides cover the wire samples, the MAC
and nonce checks, row validation and the alert rules; `python3 -m pytest gateway/tests` runs
the gateway's.

## Facts checked 2026-11-15

One live `/api/v1/alerts` request and one download of each region file showed: every alert entry
has `level` (`red` or `yellow`), `since` in RFC 3339 with fractions, and `oblast` as a full name
such as `Київська область`, matching the `region` property in `oblasts.geojson`. Alert keys match
the GeoJSON `key` values (bare names such as `вишгородський`, not the `odeska:odeskyi` form in
the developers page's example), and raion keys are unique. `oblasts.geojson` has 27 features
including `м. київ`; `raions.geojson` has 136 and none inside Kyiv city. Kyiv city's alert key is
inferred from the region file, not yet seen in an alert. Neither the developers page nor the
terms say whether the 5 s REST guideline counts per endpoint.

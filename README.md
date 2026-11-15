# alert-clock

Custom firmware for the HG V3.2.2 ESP8266 "nixie" IPS clock. It shows the time, the
air-raid alert status for your area, and a small map of drones and missiles near your
home, all in Ukrainian. A Raspberry Pi on the same network (`gateway/`) fetches NEPTUN's
alerts and tracks and OpenStreetMap's rivers and roads, does the geography, draws the map
background, and hands the clock a small signed snapshot every 2 s over plain HTTP. The
hardware facts (pins, display settings) come from the reconstruction of the supplier
firmware in the sibling `re-nixie-clock` repository.

> The map and the alerts are informational. Positions come from aggregated monitoring
> channels: they are approximate and delayed, and an empty map does not mean you are
> safe. Follow official air-raid alerts and go to a shelter when one is raised.

## What it shows

| State | Screen | LED |
| --- | --- | --- |
| No alert, nothing nearby | Clock, date and a card with the nearest track | off |
| Alert at home (red / yellow) or elsewhere in your oblast (partial) | Banner over the clock or map with how long the alert has lasted (red / yellow); beeps on change | red / amber / dim red |
| Tracks within the watch radius | Map: alerted raions and oblasts filled red or yellow, rivers, roads, towns, range rings, your location, tracks with heading arrows, trails of the last 15 min and uncertainty circles | — |
| Gateway data older than 3 minutes, or no reply from the gateway for 60 s | "Немає даних" banner; never an all-clear | orange |

At start-up the clock shows for 4 s that the source is unofficial and that official
air-raid signals come first. Short button press: show the map for 30 s (press again to go
back). Long press: poll the gateway now.

Track colours: amber drone (light amber FPV), cyan reconnaissance drone, red
missile, magenta ballistic, yellow guided bomb, white aircraft, grey unknown, grey
outline for observations that
are not a shelter signal (NEPTUN "advisory"). Tracks NEPTUN can only place in a
region ("areaOnly") are never drawn; the clock counts the ones in your oblast.

## Setup

1. The gateway, on a Raspberry Pi (Raspberry Pi OS): clone this repository anywhere under
   your account (e.g. `~/alert-clock`), run `sudo gateway/install.sh` from that account (the
   service runs as it, e.g. `pi`), set your home coordinates in `/etc/alert-gateway.toml`, then
   `sudo systemctl restart alert-gateway`. The installer generates the shared key and prints
   the `#define CLOCK_GATEWAY_KEY` line for the next step. Updates: `git pull` and the restart.
2. Install PlatformIO Core 6.2 (`uv tool install platformio` or `pipx install platformio`).
   The PC build also needs the SDL2 development headers (`pacman -S sdl2` or `apt install libsdl2-dev`).
3. `cp include/secrets.example.h include/secrets.h` and fill it in: the gateway key
   and an admin password of 8+ characters.
4. First flash, over serial. Back up the supplier firmware first:
   `pio pkg exec -p platformio/tool-esptoolpy -- esptool.py --port /dev/ttyUSB0 read_flash 0 0x400000 backup.bin`.
   The ESP8266 enters its bootloader when GPIO0 (also the display's DC line) is held
   low at reset. Then `pio run -e clock -t upload --upload-port /dev/ttyUSB0`.
   The supplier firmware's `/upload` page only stores image files, so it cannot install firmware.
5. Wi-Fi and gateway: the clock opens the network `AlertClock-Setup` (WPA2 key: the admin
   password). Join it and open http://192.168.4.1/wifi (user `admin`, the admin password);
   enter your network and the Pi's address (`host` or `host:port`, port 8080 by default).
6. Later updates go over Wi-Fi:
   `CLOCK_ADMIN_PASSWORD=... pio run -e clock-ota -t upload --upload-port <clock IP>`.
   The clock shows its IP address on screen at start-up and prints it on the serial log.

After three crashes in a row the clock starts in safe mode: only Wi-Fi, firmware
upload and the debug service run, so a bad build can always be replaced over Wi-Fi.

## Light and sound

Open `/settings` on the clock and sign in as `admin` with the same password used for
Wi-Fi setup and firmware uploads. Day and night each have screen brightness, LED intensity,
and separate start, escalation and all-clear sound toggles. LED intensity scales the status
colours; 100% retains their normal intensity. The daily schedule uses Kyiv local time and can
cross midnight. Its default is 23:00–10:00, with a 5% screen, 5% LED and silent automatic
notifications at night; day defaults are 80%, 100% and all three sounds enabled.
Before time sync, the clock uses the night profile, including startup messages.

Save persists both profiles and the schedule without restarting. Preview tests unsaved values
without changing alert state or stored settings. Explicit sound previews are audible even
when the selected sound or night sounds are disabled. One preview runs at a time, for at
most three seconds. Live events continue to arrive; after the preview, the clock restores
its current status and plays only a still-relevant, enabled pending sound. Previews are
unavailable in safe mode.

The LED pulses on alert start or escalation, then holds its status colour; it fades off on
clear and double-flashes amber every five seconds for missing data. Raised and escalation
sounds retain their distinct patterns; all-clear is a short 1,400 Hz chirp. `/debug/state`
reports the intended output values as well as gateway health. Physical light and sound must
be checked on the clock.

## Development

`AGENTS.md` holds the development loop (unit tests, PC scenarios, clock build) and
what counts as passing. Two more commands:

```sh
pio run -e clock-debug                        # firmware with heap-corruption checks and core debug output
python3 tools/mock_server.py --host 0.0.0.0   # fake NEPTUN and Overpass for a gateway on your LAN
python3 tools/make_fonts.py                   # regenerate lib/ui/src/fonts.* from Inter
python3 tools/make_wire_fixtures.py           # regenerate the C++ protocol fixtures after a protocol change
```

| Path | Contents |
| --- | --- |
| `gateway/` | The Pi gateway (Python): NEPTUN and Overpass in, signed snapshots and basemaps out. `DESIGN.md`, `PROTOCOL.md`. |
| `lib/core` | Portable clock logic: HTTP, the protocol decoder and HMAC, polling, screen rules. Builds for the clock and the PC. |
| `lib/ui` | Draws the screen with LovyanGFX through a 4-row strip (no full frame buffer fits in RAM); Inter fonts. |
| `src/device` | The clock: display, Wi-Fi, button, buzzer, LED, web service, safe mode. |
| `src/native` | The PC scenario program. |
| `fixtures/scenarios`, `tools/` | Scripted NEPTUN data, the mock server and the scenario runner. |

Memory is the main constraint. Static data takes about 59 KB of the ESP8266's 80 KB of RAM,
20 KB of it the basemap, which the clock keeps run-length encoded and decodes one 4-row strip
at a time; Wi-Fi and the web server share the rest. Plain HTTP to the Pi removes the 16.7 KB
TLS buffer the clock once needed. `/debug/state` reports free heap and the largest free block.

## Data sources and terms

- **NEPTUN** (https://neptun.in.ua/developers): free, no key. The gateway holds its
  WebSocket stream open and calls REST at most every 5 s. Its terms require a visible link
  next to the data and a statement that the source is unofficial, pointing to official
  signals: the screens' footer and the start-up notice.
- **OpenStreetMap** via Overpass (https://overpass-api.de): rivers, main roads and towns
  for the map, fetched once per home and radius and cached on the Pi. The footer credits
  "© OpenStreetMap".
- **alerts.in.ua** is no longer used: NEPTUN's alerts cover raions and oblasts.
- **war-monitor.org** has no public API: its map gates data behind Cloudflare
  Turnstile and per-browser signed sessions, so it is not used.

## Status

Verified on the PC: unit tests on both sides, and all scenarios through the real gateway
against the mock. Not yet verified:

- the gateway on a Pi against live NEPTUN and Overpass
- display orientation, colour order and the 240×240 offset (`kRotation` in `src/device/main.cpp`)
- button polarity: active low with an external pull-up is assumed
- heap headroom on the clock (`heap.free` in `/debug/state`)

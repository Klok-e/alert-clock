# alert-clock

Custom firmware for the HG V3.2.2 ESP8266 "nixie" IPS clock. It shows the time, the
air-raid alert status for your area (alerts.in.ua), and a small map of drones and
missiles near a set location (NEPTUN). The hardware facts (pins, display settings)
come from the reconstruction of the supplier firmware in the sibling
`re-nixie-clock` repository.

> The map and the alerts are informational. Positions come from aggregated monitoring
> channels: they are approximate and delayed, and an empty map does not mean you are
> safe. Follow official air-raid alerts and go to a shelter when one is raised.

## What it shows

| State | Screen | LED |
| --- | --- | --- |
| No alert, nothing nearby | Clock and date | off |
| Alert in your region (red / yellow / partial) | Banner over the clock or map; beeps on change | red / amber / dim red |
| Tracks within the watch radius | Map: range rings, your location (+), tracks pointing along their heading, uncertainty circles for approximate positions | — |
| Alert data older than 3 minutes | "NO ALERT DATA" banner; never an all-clear | orange |

Short button press: show the map for 30 s (press again to go back). Long press:
refresh both sources now.

Track colours: amber drone (light amber FPV), cyan reconnaissance drone, red
missile, magenta ballistic, yellow guided bomb, white aircraft, grey unknown, grey
outline for observations that
are not a shelter signal (NEPTUN "advisory"). Tracks NEPTUN can only place in a
region ("areaOnly") are never drawn; the clock counts the ones in your region.

## Setup

1. Install PlatformIO Core 6.2 (`uv tool install platformio` or `pipx install platformio`).
   The PC build also needs the SDL2 development headers (`pacman -S sdl2` or `apt install libsdl2-dev`).
2. `cp include/secrets.example.h include/secrets.h` and fill it in: your map centre,
   the alerts.in.ua location UID, an alerts.in.ua API token (request one at
   https://devs.alerts.in.ua), and an admin password of 8+ characters.
3. First flash, over serial. Back up the supplier firmware first:
   `pio pkg exec -p platformio/tool-esptoolpy -- esptool.py --port /dev/ttyUSB0 read_flash 0 0x400000 backup.bin`.
   The ESP8266 enters its bootloader when GPIO0 (also the display's DC line) is held
   low at reset. Then `pio run -e clock -t upload --upload-port /dev/ttyUSB0`.
   The supplier firmware's `/upload` page only stores image files, so it cannot install firmware.
4. Wi-Fi: the clock opens the network `AlertClock-Setup` (WPA2 key: the admin
   password). Join it and open http://192.168.4.1/wifi (user `admin`, the admin password).
5. Later updates go over Wi-Fi:
   `CLOCK_ADMIN_PASSWORD=... pio run -e clock-ota -t upload --upload-port <clock IP>`.
   The clock shows its IP address on screen at start-up and prints it on the serial log.

After three crashes in a row the clock starts in safe mode: only Wi-Fi, firmware
upload and the debug service run, so a bad build can always be replaced over Wi-Fi.

## Development

`AGENTS.md` holds the development loop (unit tests, PC scenarios, clock build) and
what counts as passing. Two more commands:

```sh
pio run -e clock-debug                        # firmware with heap-corruption checks and core debug output
python3 tools/mock_server.py --host 0.0.0.0   # the mock APIs, reachable by the clock on your LAN
```

| Path | Contents |
| --- | --- |
| `lib/core` | Portable logic: HTTP, parsers, scheduling, screen rules. Builds for the clock and the PC. |
| `lib/ui` | Draws the screen with LovyanGFX, through a 4-row strip (no full frame buffer fits in RAM). |
| `src/device` | The clock: display, Wi-Fi, TLS, button, buzzer, LED, web service, safe mode. |
| `src/native` | The PC scenario runner. |
| `fixtures/scenarios`, `tools/mock_server.py` | Scripted API data for the loop. |

Memory is the main constraint: static data takes about half of the ESP8266's 80 KB of
RAM, Wi-Fi and TLS share the rest, and an HTTPS fetch needs a 16.7 KB contiguous block
(the APIs do not negotiate smaller TLS records). `/debug/state` reports free heap, the
largest free block and the parse arena's high-water mark.

## Data sources and terms

- **NEPTUN** (https://neptun.in.ua/developers): free, no key. Poll at most every 5 s
  (the clock uses 15 s) and keep the visible "data: NEPTUN" credit on the map.
- **alerts.in.ua** (https://devs.alerts.in.ua): token required; 8–10 requests a minute
  per IP (the clock uses one every 20 s and backs off a minute after a 429).
- **war-monitor.org** has no public API: its map gates data behind Cloudflare
  Turnstile and per-browser signed sessions, so it is not used.

## Status

Verified on the PC: unit tests, all scenarios, and parsing a real NEPTUN snapshot.
Not yet verified on the clock:

- display orientation, colour order and the 240×240 offset (`kRotation` in `src/device/main.cpp`)
- button polarity: active low with an external pull-up is assumed
- heap headroom during HTTPS fetches (watch `heap.max_block` in `/debug/state`)
- real alerts.in.ua responses

Not built yet: a Ukrainian (Cyrillic) font, a basemap behind the tracks, a settings
page (home location and token are compile-time), night dimming.

# alert-clock

`README.md` covers setup, why RAM is tight, and the data-source terms that bind any
change to fetching or to the map screen.

The clock gets everything from a Raspberry Pi gateway (`gateway/`). Read `gateway/DESIGN.md`
before changing fetching, alert rules, the screen data, the basemap or the RAM rules below, and
`gateway/PROTOCOL.md` before changing the bytes between the gateway and the clock.

## Verification

After every code or fixture change, run:

1. `pio test -e native` passes when every test case passes with no sanitizer report.
2. `python3 -m pytest gateway/tests` passes with no failures.
3. `python3 tools/run_scenarios.py` passes when it ends with `N/N scenarios passed`.
   Then open the PNG frame of every step your change can affect (the run ends by
   naming the frames directory). Frames show the layout as the clock composes it;
   orientation, colour order and offset on the real panel need a photo of the clock
   from the user, since the screen cannot be read back.
4. `pio run -e clock` passes on SUCCESS with the `RAM:` line at or below 57,000 bytes.
   Note the value before you change code; when your change raises it, report both
   values and the cause.

New screen behaviour gets a scenario in `fixtures/scenarios/` (expectation keys:
`panel`, `banner`, `sound`, `markers_min`, `markers_max`, `basemap`; a step with
`"notice": true` draws the start-up notice); its `home` and `start` must fit the constants
commented at the top of `tools/mock_server.py`. New NEPTUN data shapes get a phase in that file.

## Rules

- Keep tests and verification runs on the mock server. Reach the real APIs only from the
  gateway or in a single request a task needs, such as checking the parser against a live
  response.
- Allocate at start-up; the firmware's main loop runs on fixed-capacity storage:
  `etl::vector`, `etl::string` and static buffers.
- Reach elements through ETL containers or bounds-checked loops.
- `lib/core` stays free of Arduino and LovyanGFX headers.
- Include ArduinoJson and LovyanGFX only through `src/native/json.h` and `lib/ui/src/gfx.h`,
  the only files that suppress warnings; fix every other warning at its cause.
- Long constant strings on the clock go in flash (`PROGMEM` with `snprintf_P`, `send_P` or
  `ui::flash_copy`): plain literals occupy RAM on the ESP8266.

## On the clock

Use the clock only when the user says it is connected, and ask the user for its IP
address and admin password (README "Setup" says where both appear). Its web routes and
their auth are listed in `src/device/service.h`.

- After `POST /debug/refresh` and a few seconds, `GET /debug/state` must show
  `gateway.outcome` `ok` and `heap.free` of at least 12,000.
- To feed the clock scenario data, run `tools/mock_server.py --host 0.0.0.0` and a gateway
  configured like the one `tools/run_scenarios.py` writes, but with `listen = "0.0.0.0"` and
  the clock's `CLOCK_GATEWAY_KEY`; then point the clock at it with
  `POST /debug/sources?gateway=<PC address>:<port>`.

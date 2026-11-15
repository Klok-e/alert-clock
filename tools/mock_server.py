#!/usr/bin/env python3
"""Mock NEPTUN and alerts.in.ua APIs for the PC build and for the clock itself.

All data is synthetic and generated here. The current phase decides every response.
Switch it with GET /_phase/<name>.

    python3 tools/mock_server.py --port 8787          # serve until Ctrl-C
    curl http://127.0.0.1:8787/_phase/drones_near     # switch phase

Routes (HTTP/1.0, like the clock's requests):
    GET /api/v1/threats                           NEPTUN threats snapshot (ETag / 304)
    GET /v2/iot/active_air_raid_alerts/<uid>.json alerts.in.ua status letter (needs a Bearer token)
    GET /_phase/<name>                            switch phase; GET /_phase lists them
"""

import argparse
import hashlib
import json
import math
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Must match "home" in fixtures/scenarios/*.json.
HOME = (50.4501, 30.5234)
HOME_REGION = "м. Київ"
# When every track was observed. A scenario's "start" must fall within a minute after it:
# the clock extrapolates positions from this time, so later starts move tracks.
OBSERVED_AT = "2026-11-14T18:59:30Z"


def destination(lat, lon, bearing_deg, km):
    """Point `km` from (lat, lon) along `bearing_deg` on a sphere."""
    r = 6371.0
    d = km / r
    b = math.radians(bearing_deg)
    lat1, lon1 = math.radians(lat), math.radians(lon)
    lat2 = math.asin(math.sin(lat1) * math.cos(d) + math.cos(lat1) * math.sin(d) * math.cos(b))
    lon2 = lon1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(lat1),
                             math.cos(d) - math.sin(lat1) * math.sin(lat2))
    return round(math.degrees(lat2), 5), round(math.degrees(lon2), 5)


def track(track_id, kind, bearing_from_home, km, heading=None, speed=150, quality="confirmed",
          uncertainty=4, advisory=False, area_only=False, region="Київська область", status="active"):
    """A track shaped like NEPTUN's, placed relative to HOME. heading=None means unknown."""
    lat, lon = destination(*HOME, bearing_from_home, km)
    item = {
        "id": track_id, "type": kind, "title": kind, "region": region, "district": "",
        "locality": "", "lat": lat, "lon": lon, "heading": heading,
        "confidenceLevel": "medium", "sourceCount": 2, "updatedAt": OBSERVED_AT,
        "explanationShort": "synthetic track for tests", "status": status,
        "confirmedAt": OBSERVED_AT, "uncertaintyKm": uncertainty, "positionQuality": quality,
        "lifecycle": "confirmed", "displayConfidence": "medium",
        "trail": [{"lat": lat, "lon": lon, "t": OBSERVED_AT}],
        "destination": {"lat": HOME[0], "lon": HOME[1], "place": "test"},
    }
    if heading is not None:
        item["velocity"] = {"bearingDeg": heading, "speedKmh": speed}
    if advisory:
        item["advisory"] = True
    if area_only:
        item.update(areaOnly=True, positionQuality="approx", heading=None, district="")
        item.pop("velocity", None)
        item.pop("trail", None)
    return item


def toward_home(bearing_from_home):
    return (bearing_from_home + 180) % 360


def far_tracks(count):
    """Tracks 300-600 km away, outside any map radius."""
    return [track(f"far_{i}", "uav", (i * 37) % 360, 300 + (i * 13) % 300, heading=(i * 53) % 360)
            for i in range(count)]


NEAR_DRONES = [
    track("near_1", "uav", 20, 18, heading=toward_home(20)),
    track("near_2", "uav", 200, 12, heading=toward_home(200), quality="approx", uncertainty=6),
    track("near_3", "recon", 300, 22, heading=90),
    track("advisory_1", "mig31k", 90, 15, heading=270, advisory=True),
    track("area_1", "missile", 0, 0, area_only=True, region=HOME_REGION),
]

PHASES = {
    "quiet": {"threats": far_tracks(12), "alert": "N"},
    "drones_near": {"threats": far_tracks(12) + NEAR_DRONES, "alert": "Y"},
    "alert_red": {"threats": far_tracks(12) + NEAR_DRONES
                  + [track("missile_1", "missile", 120, 28, heading=toward_home(120), speed=800)],
                  "alert": "R"},
    "advisory_only": {"threats": [track("advisory_2", "mig31k", 45, 10, heading=200, advisory=True)],
                      "alert": "N"},
    "big_feed": {"threats": far_tracks(800) + NEAR_DRONES[:2], "alert": "R"},
    "malformed": {"threats_raw": b'{"epoch":1,"threats":[{"id":"x","type":"uav","lat":50.5,',
                  "alert": "N"},
    "outage": {"threats_status": 503, "alert_status": 503},
}


def threats_body(phase):
    doc = {"epoch": 1, "seq": 1, "serverTime": OBSERVED_AT, "threats": phase["threats"]}
    return json.dumps(doc, ensure_ascii=False).encode("utf-8")


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.phase = "quiet"


ALERT_PATH = re.compile(r"^/v2/iot/active_air_raid_alerts/\d+\.json$")


def make_handler(state, verbose):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *args):
            if verbose:
                sys.stderr.write("mock: " + fmt % args + "\n")

        def reply(self, status, body=b"", content_type="application/json", headers=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            with state.lock:
                phase_name = state.phase
            phase = PHASES[phase_name]

            if path == "/_phase":
                return self.reply(200, json.dumps({"phase": phase_name, "phases": sorted(PHASES)}).encode())
            if path.startswith("/_phase/"):
                name = path[len("/_phase/"):]
                if name not in PHASES:
                    return self.reply(404, b"unknown phase\n", "text/plain")
                with state.lock:
                    state.phase = name
                return self.reply(200, b"ok\n", "text/plain")

            if path == "/api/v1/threats":
                if "threats_status" in phase:
                    return self.reply(phase["threats_status"], b"unavailable\n", "text/plain")
                body = phase.get("threats_raw") or threats_body(phase)
                etag = '"' + hashlib.sha1(body).hexdigest()[:16] + '"'
                if self.headers.get("If-None-Match") == etag:
                    return self.reply(304, headers={"ETag": etag})
                return self.reply(200, body, headers={"ETag": etag})

            if ALERT_PATH.match(path):
                if not (self.headers.get("Authorization") or "").startswith("Bearer "):
                    return self.reply(401, b'{"message":"token required"}')
                if "alert_status" in phase:
                    return self.reply(phase["alert_status"], b'{"message":"unavailable"}')
                return self.reply(200, json.dumps(phase["alert"]).encode())

            return self.reply(404, b"not found\n", "text/plain")

    return Handler


def make_server(host="127.0.0.1", port=0, verbose=False):
    """Returns a ThreadingHTTPServer; port 0 picks a free port (see server.server_address)."""
    return ThreadingHTTPServer((host, port), make_handler(State(), verbose))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1",
                        help="0.0.0.0 lets the clock reach it over the LAN")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    server = make_server(args.host, args.port, args.verbose)
    print(f"mock server on http://{args.host}:{server.server_address[1]} (phases: {', '.join(sorted(PHASES))})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

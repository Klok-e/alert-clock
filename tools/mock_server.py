#!/usr/bin/env python3
"""Fake NEPTUN and Overpass for the gateway's tests and scenario runs.

All data is synthetic and generated here. The current phase decides every response; switch it
with GET /_phase/<name> (GET /_phase lists them). A phase switch pushes a fresh snapshot and
alerts state to every open stream.

    python3 tools/mock_server.py --port 8787           # serve until Ctrl-C
    curl http://127.0.0.1:8787/_phase/drones_near      # switch phase

Routes:
    GET  /api/v1/threats, /api/v1/alerts       NEPTUN REST snapshots
    GET  /api/v1/stream                        NEPTUN WebSocket: snapshot, alerts, heartbeats
    GET  /oblasts.geojson, /raions.geojson     synthetic region outlines around Kyiv
    POST /api/interpreter                      Overpass: a river, a road and two towns
    GET  /_phase/<name>, /_phase               switch or list phases
"""

import argparse
import asyncio
import json
import math
import threading

from aiohttp import web

# Scenario "home" must be one of these, and "start" within a minute after OBSERVED_AT: the
# gateway dead-reckons positions from it, so later starts move tracks.
HOME = (50.4501, 30.5234)            # inside the synthetic "м. київ" (no raions)
HOME_VYSHHOROD = (50.85, 30.45)      # inside the synthetic "вишгородський" raion
OBSERVED_AT = "2026-11-14T18:59:30Z"
OBSERVED_UNIX = 1794682770
HEARTBEAT_S = 1.0


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


def iso(unix):
    import datetime
    return datetime.datetime.fromtimestamp(unix, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def track(track_id, kind, bearing_from_home, km, heading=None, speed=150, quality="confirmed",
          uncertainty=4, advisory=False, area_only=False, region="Київська область", region_key="київська",
          status="active", home=HOME):
    """A track shaped like NEPTUN's, placed relative to `home`. heading=None means unknown."""
    lat, lon = destination(*home, bearing_from_home, km)
    trail = [{"lat": lat, "lon": lon, "t": OBSERVED_AT}]
    if heading is not None:
        for minutes in (1, 2, 3, 4):  # where it came from, a minute apart
            plat, plon = destination(lat, lon, (heading + 180) % 360, speed * minutes / 60)
            trail.insert(0, {"lat": plat, "lon": plon, "t": iso(OBSERVED_UNIX - 60 * minutes)})
    item = {
        "id": track_id, "type": kind, "title": kind, "region": region, "regionKey": region_key,
        "district": "", "locality": "", "lat": lat, "lon": lon, "heading": heading,
        "confidenceLevel": "medium", "sourceCount": 2, "updatedAt": OBSERVED_AT,
        "explanationShort": "synthetic track for tests", "status": status,
        "confirmedAt": OBSERVED_AT, "uncertaintyKm": uncertainty, "positionQuality": quality,
        "lifecycle": "confirmed", "displayConfidence": "medium", "trail": trail,
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
    return [track(f"far_{i}", "uav", (i * 37) % 360, 300 + (i * 13) % 300, heading=(i * 53) % 360,
                  region="Харківська область", region_key="харківська")
            for i in range(count)]


NEAR_DRONES = [
    track("near_1", "uav", 20, 18, heading=toward_home(20)),
    track("near_2", "uav", 200, 12, heading=toward_home(200), quality="approx", uncertainty=6),
    track("near_3", "recon", 300, 22, heading=90),
    track("advisory_1", "mig31k", 90, 15, heading=270, advisory=True),
    track("area_1", "missile", 0, 0, area_only=True, region="м. Київ", region_key="м. київ"),
]


def entry(key, name, oblast, level="red"):
    item = {"key": key, "name": name, "oblast": oblast, "since": "2026-11-14T18:50:00.123456Z"}
    if level is not None:
        item["level"] = level
    return item


FAR_ALERT = entry("луганська", "Луганська область", "Луганська область")
KYIV_CITY = lambda level: entry("м. київ", "м. Київ", "м. Київ", level)  # noqa: E731
QUIET_ALERTS = {"raions": [], "oblasts": [FAR_ALERT]}

MOTION_TRACK = track("motion", "uav", 0, 10, heading=90, quality="approx", uncertainty=6)
MOTION_TRACK["trail"] = []
for i in range(11):
    lat, lon = destination(*HOME, 300 + i * 6, 22 - i * 1.2)
    MOTION_TRACK["trail"].append({"lat": lat, "lon": lon, "t": iso(OBSERVED_UNIX - (10 - i) * 60)})
STATIONARY_TRACK = track("no-speed", "uav", 0, 10, heading=90, quality="approx", uncertainty=6)
STATIONARY_TRACK.pop("velocity")
STATIONARY_TRACK["trail"] = [MOTION_TRACK["trail"][0], MOTION_TRACK["trail"][6]]

PHASES = {
    "motion_paths": {"threats": [MOTION_TRACK],
                     "alerts": {"raions": [], "oblasts": [KYIV_CITY("red")]}},
    "missing_speed": {"threats": [STATIONARY_TRACK], "alerts": QUIET_ALERTS},
    "quiet": {"threats": far_tracks(12), "alerts": QUIET_ALERTS},
    "drones_near": {"threats": far_tracks(12) + NEAR_DRONES,
                    "alerts": {"raions": [], "oblasts": [FAR_ALERT, KYIV_CITY("yellow")]}},
    "alert_red": {"threats": far_tracks(12) + NEAR_DRONES
                  + [track("missile_1", "missile", 120, 28, heading=toward_home(120), speed=800)],
                  "alerts": {"raions": [], "oblasts": [FAR_ALERT, KYIV_CITY("red")]}},
    "advisory_only": {"threats": [track("advisory_2", "mig31k", 45, 10, heading=200, advisory=True)],
                      "alerts": QUIET_ALERTS},
    "big_feed": {"threats": far_tracks(800) + NEAR_DRONES[:2],
                 "alerts": {"raions": [], "oblasts": [FAR_ALERT, KYIV_CITY("red")]}},
    "malformed": {"threats_raw": b'{"threats":[{"id":"x","type":"uav","lat":50.5,',
                  "alerts": QUIET_ALERTS, "stream_raw": '{"type":"snapshot","data":{"threats":[{"id"'},
    "outage": {"status": 503},
    # Home in the Vyshhorod raion (HOME_VYSHHOROD).
    "oblast_yellow": {"threats": [], "alerts": {"raions": [], "oblasts": [
        FAR_ALERT, entry("київська", "Київська область", "Київська область", "yellow")]}},
    "raion_red": {"threats": [track("v_1", "uav", 300, 9, heading=120, home=HOME_VYSHHOROD)],
                  "alerts": {"raions": [entry("вишгородський", "Вишгородський район", "Київська область")],
                             "oblasts": [FAR_ALERT]}},
    "partial": {"threats": [], "alerts": {"raions": [
        entry("броварський", "Броварський район", "Київська область")], "oblasts": [FAR_ALERT]}},
    "missing_level": {"threats": [], "alerts": {"raions": [
        entry("вишгородський", "Вишгородський район", "Київська область", None)], "oblasts": [FAR_ALERT]}},
}


def box(south, west, north, east):
    return [[west, south], [east, south], [east, north], [west, north], [west, south]]


CITY_BOX = (50.33, 30.32, 50.57, 30.72)
OBLASTS = {"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {"key": "київська", "region": "Київська область"},
     "geometry": {"type": "Polygon", "coordinates": [box(49.5, 29.3, 51.5, 32.2), box(*CITY_BOX)]}},
    {"type": "Feature", "properties": {"key": "м. київ", "name": "м. Київ"},
     "geometry": {"type": "Polygon", "coordinates": [box(*CITY_BOX)]}},
    {"type": "Feature", "properties": {"key": "чернігівська", "region": "Чернігівська область"},
     "geometry": {"type": "Polygon", "coordinates": [box(51.5, 30.5, 52.4, 33.0)]}},
    {"type": "Feature", "properties": {"key": "луганська", "region": "Луганська область"},
     "geometry": {"type": "Polygon", "coordinates": [box(48.0, 38.0, 50.0, 40.0)]}},
]}
RAIONS = {"type": "FeatureCollection", "features": [
    {"type": "Feature", "properties": {"key": key, "rayon": name},
     "geometry": {"type": "Polygon", "coordinates": [box(*bounds)]}}
    for key, name, bounds in [
        ("вишгородський", "Вишгородський район", (50.6, 29.8, 51.5, 30.9)),
        ("броварський", "Броварський район", (50.2, 30.9, 51.2, 32.2)),
        ("бучанський", "Бучанський район", (50.2, 29.3, 50.6, 30.3)),
        ("обухівський", "Обухівський район", (49.6, 30.2, 50.2, 31.3)),
    ]]}


def overpass_answer():
    """A river through both homes, one primary road and two towns."""
    river = [destination(*HOME, 0, km) for km in range(-60, 61, 5)]
    road = [destination(*HOME, 70, km) for km in range(-50, 51, 10)]
    return {"elements": [
        {"type": "way", "tags": {"waterway": "river", "name": "Дніпро"},
         "geometry": [{"lat": lat, "lon": lon + 0.05} for lat, lon in river]},
        {"type": "way", "tags": {"highway": "primary"}, "geometry": [{"lat": a, "lon": b} for a, b in road]},
        {"type": "node", "lat": 50.511, "lon": 30.790, "tags": {"place": "town", "name:uk": "Бровари"}},
        {"type": "node", "lat": 50.584, "lon": 30.488, "tags": {"place": "town", "name:uk": "Вишгород"}},
    ]}


class Mock:
    def __init__(self):
        self.phase = "quiet"
        self.sockets: set[web.WebSocketResponse] = set()

    def current(self):
        return PHASES[self.phase]

    async def threats(self, request):
        phase = self.current()
        if "status" in phase:
            raise web.HTTPServiceUnavailable()
        if "threats_raw" in phase:
            return web.Response(body=phase["threats_raw"], content_type="application/json")
        return web.json_response({"version": 1, "updatedAt": OBSERVED_AT, "threats": phase["threats"]})

    async def alerts(self, request):
        phase = self.current()
        if "status" in phase:
            raise web.HTTPServiceUnavailable()
        return web.json_response({"version": 1, "updatedAt": OBSERVED_AT, **phase["alerts"]})

    async def push_state(self, ws):
        phase = self.current()
        if "stream_raw" in phase:
            await ws.send_str(phase["stream_raw"])
        elif "threats" in phase:
            await ws.send_str(json.dumps({"type": "snapshot", "data": {"threats": phase["threats"]}},
                                         ensure_ascii=False))
        if "alerts" in phase:
            await ws.send_str(json.dumps({"type": "alerts", "data": phase["alerts"]}, ensure_ascii=False))

    async def stream(self, request):
        if "status" in self.current():
            raise web.HTTPServiceUnavailable()
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.sockets.add(ws)
        try:
            await self.push_state(ws)
            while not ws.closed:
                await asyncio.sleep(HEARTBEAT_S)
                if not ws.closed:
                    await ws.send_str('{"type":"heartbeat"}')
        except (ConnectionResetError, RuntimeError):
            pass
        finally:
            self.sockets.discard(ws)
        return ws

    async def set_phase(self, request):
        name = request.match_info["name"]
        if name not in PHASES:
            raise web.HTTPNotFound(text="unknown phase\n")
        self.phase = name
        for ws in list(self.sockets):
            try:
                if "status" in self.current():
                    await ws.close()
                else:
                    await self.push_state(ws)
            except (ConnectionResetError, RuntimeError):  # a gateway that has exited
                self.sockets.discard(ws)
        return web.Response(text="ok\n")

    async def list_phases(self, request):
        return web.json_response({"phase": self.phase, "phases": sorted(PHASES)})

    def app(self):
        app = web.Application()
        app.router.add_get("/api/v1/threats", self.threats)
        app.router.add_get("/api/v1/alerts", self.alerts)
        app.router.add_get("/api/v1/stream", self.stream)
        app.router.add_get("/oblasts.geojson", lambda r: web.json_response(OBLASTS))
        app.router.add_get("/raions.geojson", lambda r: web.json_response(RAIONS))
        app.router.add_post("/api/interpreter", lambda r: web.json_response(overpass_answer()))
        app.router.add_get("/_phase", self.list_phases)
        app.router.add_get("/_phase/{name}", self.set_phase)
        return app


class Server:
    """The mock on its own event loop in a background thread."""

    def __init__(self, host="127.0.0.1", port=0):
        self.loop = asyncio.new_event_loop()
        self.runner = web.AppRunner(Mock().app(), access_log=None)
        self.loop.run_until_complete(self.runner.setup())
        self.site = web.TCPSite(self.runner, host, port)
        self.loop.run_until_complete(self.site.start())
        self.port = self.runner.addresses[0][1]
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()

    def stop(self):
        asyncio.run_coroutine_threadsafe(self.runner.cleanup(), self.loop).result(10)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1", help="0.0.0.0 lets a Pi on the LAN reach it")
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    print(f"mock NEPTUN/Overpass on http://{args.host}:{args.port} (phases: {', '.join(sorted(PHASES))})")
    web.run_app(Mock().app(), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()

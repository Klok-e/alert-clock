"""NEPTUN upstream: the WebSocket stream, paced REST requests and per-source freshness.

Stream messages (checked against the live service 2026-11-15): on connect a "snapshot"
({"data": {"threats": [...]}}) and an "alerts" state; then per-track "upsert" ({"data": track})
and "remove" ({"data": {"id": ...}}), a full "alerts" state on every change, and a
"heartbeat" about every 14 s.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import aiohttp

from .config import Config

log = logging.getLogger(__name__)

REST_SPACING_S = 5.0  # NEPTUN: poll REST at most once every 5 s
REST_REFRESH_S = 300.0  # while the stream is up
REST_POLL_DOWN_S = 30.0  # while the stream is down
RECONNECT_MIN_S = 5.0
RECONNECT_MAX_S = 300.0
STABLE_S = 3600.0
REGION_REFRESH_S = 86400.0
STALE_AFTER_S = 180.0
SOURCES = ("threats", "alerts")
REGION_FILES = ("oblasts", "raions")  # separate queue items, so each request is paced


def check_regions(body: object) -> None:
    """Raises ValueError unless `body` is a FeatureCollection the gateway can use."""
    if not isinstance(body, dict) or not isinstance(body.get("features"), list):
        raise ValueError("not a FeatureCollection")
    for feature in body["features"]:
        properties = feature.get("properties") if isinstance(feature, dict) else None
        geometry = feature.get("geometry") if isinstance(feature, dict) else None
        if (not isinstance(properties, dict) or not isinstance(properties.get("key"), str)
                or not isinstance(geometry, dict) or geometry.get("type") not in ("Polygon", "MultiPolygon")
                or not isinstance(geometry.get("coordinates"), list)):
            raise ValueError("a feature without a key or a polygon")


class Clock:
    """Wall-clock time; the --test endpoint can set it."""

    def __init__(self) -> None:
        self._fixed: float | None = None

    def now(self) -> float:
        return self._fixed if self._fixed is not None else time.time()

    def set(self, unix: float) -> None:
        self._fixed = unix


@dataclass
class SourceState:
    last_good: float | None = None
    rest_ok: bool = False  # the latest REST fetch on the current connection succeeded

    def age(self, now: float) -> float:
        return float("inf") if self.last_good is None else max(0.0, now - self.last_good)

    def stale(self, now: float) -> bool:
        return self.age(now) > STALE_AFTER_S


def parse_alerts(body: object) -> dict:
    if not isinstance(body, dict):
        raise ValueError("alerts must be an object")
    parsed = {}
    for name in ("raions", "oblasts"):
        entries = body.get(name)
        if not isinstance(entries, list):
            raise ValueError(f"alerts must contain a {name} array")
        if any(not isinstance(entry, dict) or not isinstance(entry.get("key"), str) for entry in entries):
            raise ValueError(f"each {name} entry must be an object with a string key")
        parsed[name] = list(entries)
    return parsed


class Neptun:
    def __init__(self, config: Config, clock: Clock, on_change: Callable[[], None]):
        self.config = config
        self.clock = clock
        self.on_change = on_change
        self.threats: dict[str, dict] = {}
        self.alerts: dict = {"raions": [], "oblasts": []}
        self.sources = {name: SourceState() for name in SOURCES}
        self.oblasts_geojson: dict | None = None
        self.raions_geojson: dict | None = None
        self.stream_up = False
        self._queue: list[str] = []
        self._queue_event = asyncio.Event()
        self._session: aiohttp.ClientSession | None = None

    # --- state updates ---------------------------------------------------------------

    def _good(self, source: str) -> None:
        self.sources[source].last_good = self.clock.now()

    def apply_threats(self, threats: list) -> None:
        self.threats = {str(t.get("id")): t for t in threats if isinstance(t, dict)}

    def apply_alerts(self, alerts: object) -> None:
        self.alerts = parse_alerts(alerts)

    def handle_frame(self, text: str) -> None:
        try:
            frame = json.loads(text)
            kind, data = frame.get("type"), frame.get("data")
        except (ValueError, AttributeError):
            log.warning("ignoring a malformed stream frame")
            return
        if kind == "snapshot" and isinstance(data, dict) and isinstance(data.get("threats"), list):
            self.apply_threats(data["threats"])
            self._good("threats")
        elif kind == "upsert" and isinstance(data, dict) and "id" in data:
            self.threats[str(data["id"])] = data
            self._good("threats")
        elif kind == "remove" and isinstance(data, dict):
            self.threats.pop(str(data.get("id")), None)
            self._good("threats")
        elif kind == "alerts":
            try:
                self.apply_alerts(data)
            except ValueError as error:
                log.warning("ignoring malformed alerts stream frame: %s", error)
                return
            self._good("alerts")
            self.on_change()
        elif kind == "heartbeat":
            for name, source in self.sources.items():
                if source.rest_ok:
                    self._good(name)

    # --- HTTP -------------------------------------------------------------------------

    async def _get_json(self, path: str) -> object:
        assert self._session is not None
        async with self._session.get(self.config.neptun_url + path, timeout=aiohttp.ClientTimeout(total=30)) as r:
            r.raise_for_status()
            return json.loads(await r.read())

    async def fetch_source(self, source: str) -> bool:
        """One REST fetch; updates the state and the source's rest_ok flag."""
        try:
            if source == "threats":
                body = await self._get_json("/api/v1/threats")
                if not isinstance(body, dict) or not isinstance(body.get("threats"), list):
                    raise ValueError("no threats array")
                self.apply_threats(body["threats"])
            else:
                body = await self._get_json("/api/v1/alerts")
                self.apply_alerts(body)
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as error:
            log.warning("REST %s failed: %s", source, error)
            self.sources[source].rest_ok = False
            return False
        self.sources[source].rest_ok = True
        self._good(source)
        if source == "alerts":
            self.on_change()
        return True

    @property
    def regions_ready(self) -> bool:
        return self.oblasts_geojson is not None and self.raions_geojson is not None

    async def fetch_region(self, name: str, cache_dir: Path) -> bool:
        """One region file ("oblasts" or "raions") from NEPTUN, else the cached copy; True if loaded."""
        path = cache_dir / f"{name}.geojson"
        try:
            body = await self._get_json(f"/{name}.geojson")
            check_regions(body)
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as error:
            log.warning("region file %s: %s; using the cache", name, error)
            try:
                body = json.loads(path.read_text())
                check_regions(body)
            except (OSError, ValueError):
                return False
        else:
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(body, ensure_ascii=False))
            except OSError as error:
                log.warning("cannot cache region file %s: %s", name, error)
        setattr(self, f"{name}_geojson", body)
        return True

    async def fetch_regions(self, cache_dir: Path) -> bool:
        """Test mode: both region files at once, ignoring the pacing."""
        for name in REGION_FILES:
            await self.fetch_region(name, cache_dir)
        return self.regions_ready

    def queue(self, *items: str) -> None:
        for item in items:
            if item not in self._queue:
                self._queue.append(item)
        self._queue_event.set()

    async def _rest_worker(self, regions_loaded: Callable[[], None]) -> None:
        """Starts queued requests at least REST_SPACING_S apart."""
        while True:
            while not self._queue:
                self._queue_event.clear()
                await self._queue_event.wait()
            item = self._queue.pop(0)
            if item in REGION_FILES:
                if await self.fetch_region(item, self.config.cache_dir) and self.regions_ready:
                    regions_loaded()
            else:
                await self.fetch_source(item)
            await asyncio.sleep(REST_SPACING_S)

    async def _schedule(self) -> None:
        last_refresh = 0.0
        while True:
            now = time.monotonic()
            interval = REST_REFRESH_S if self.stream_up else REST_POLL_DOWN_S
            if now - last_refresh >= interval:
                self.queue("threats", "alerts")
                if not self.regions_ready:  # a failed start-up fetch, with no cache
                    self.queue(*REGION_FILES)
                last_refresh = now
            await asyncio.sleep(1)

    async def _regions_daily(self) -> None:
        while True:
            await asyncio.sleep(REGION_REFRESH_S)
            self.queue(*REGION_FILES)

    async def _stream(self) -> None:
        delay = RECONNECT_MIN_S
        while True:
            opened = time.monotonic()
            try:
                assert self._session is not None
                async with self._session.ws_connect(self.config.neptun_stream, heartbeat=None) as ws:
                    self.stream_up = True
                    for source in self.sources.values():
                        source.rest_ok = False
                    self.queue("threats", "alerts")
                    log.info("stream connected")
                    while True:
                        message = await ws.receive(timeout=60)  # heartbeats come every ~14 s
                        if message.type == aiohttp.WSMsgType.TEXT:
                            self.handle_frame(message.data)
                        elif message.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED,
                                              aiohttp.WSMsgType.ERROR):
                            break
            except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as error:
                log.warning("stream: %s", error)
            self.stream_up = False
            for source in self.sources.values():
                source.rest_ok = False
            if time.monotonic() - opened >= STABLE_S:
                delay = RECONNECT_MIN_S
            await asyncio.sleep(delay)
            delay = min(delay * 2, RECONNECT_MAX_S)

    async def run(self, regions_loaded: Callable[[], None]) -> None:
        headers = {"User-Agent": self.config.user_agent}
        async with aiohttp.ClientSession(headers=headers) as session:
            self._session = session
            self.queue(*REGION_FILES)
            await asyncio.gather(self._rest_worker(regions_loaded), self._schedule(), self._stream(),
                                 self._regions_daily())

    async def refresh_now(self) -> None:
        """Test mode: both REST sources at once, ignoring the pacing."""
        for source in SOURCES:
            await self.fetch_source(source)

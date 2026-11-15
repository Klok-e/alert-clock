"""The gateway: NEPTUN and Overpass in, signed snapshots and basemaps out."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

from aiohttp import web

from . import basemap, config as config_module, overpass, regions, tracks, wire
from .geo import choose_rings, HALF
from .neptun import Clock, Neptun

log = logging.getLogger("alert_gateway")


class Gateway:
    def __init__(self, config: config_module.Config, test_mode: bool):
        self.config = config
        self.test_mode = test_mode
        self.clock = Clock()
        self.neptun = Neptun(config, self.clock, self._alerts_changed)
        self.home: regions.HomeRegion | None = None
        self.osm: dict | None = overpass.load_cached(config)
        self.basemap: basemap.Basemap | None = None
        self._fills: tuple[dict, dict] | None = None
        self.rings = choose_rings(config.map_radius_km)
        self.px_per_km = HALF / config.map_radius_km

    # --- region and basemap ----------------------------------------------------------

    def regions_loaded(self) -> None:
        n = self.neptun
        if n.oblasts_geojson is None or n.raions_geojson is None:
            return
        home = regions.resolve_home(n.oblasts_geojson, n.raions_geojson, *self.config.home)
        if home is None:
            log.error("home %.5f, %.5f is in no oblast of NEPTUN's region files", *self.config.home)
            raise SystemExit(2)
        if home != self.home:
            log.info("home region: oblast %s, raion %s", home.oblast_key, home.raion_key or "-")
        self.home = home
        self._rebuild_basemap(force=True)

    def _alerts_changed(self) -> None:
        self._rebuild_basemap(force=False)

    def _current_fills(self) -> tuple[dict, dict]:
        if self.neptun.sources["alerts"].stale(self.clock.now()):
            return ({}, {})
        return regions.fills(self.neptun.alerts)

    def _rebuild_basemap(self, force: bool) -> None:
        n = self.neptun
        if self.home is None or n.oblasts_geojson is None or n.raions_geojson is None:
            return
        fills = self._current_fills()
        if not force and fills == self._fills:
            return
        self._fills = fills
        label = self.home.oblast_name if self.home.raion_key is None else self._raion_name()
        try:
            self.basemap = basemap.render(n.oblasts_geojson, n.raions_geojson, fills[0], fills[1], self.osm,
                                          self.config.home, self.config.map_radius_km, label,
                                          self.config.label_font)
        except config_module.ConfigError as error:
            log.error("%s", error)
            raise SystemExit(2) from error

    def _raion_name(self) -> str:
        for feature in (self.neptun.raions_geojson or {}).get("features", []):
            if feature["properties"].get("key") == (self.home.raion_key if self.home else None):
                return str(feature["properties"].get("rayon") or feature["properties"]["key"])
        return ""

    async def _watch_fills(self) -> None:
        """Redraws when the alerts source goes stale (fills cleared) or fresh again."""
        while True:
            await asyncio.sleep(5)
            self._rebuild_basemap(force=False)

    async def _overpass(self) -> None:
        if self.osm is not None:
            return
        self.osm = await overpass.fetch_until_cached(self.config)
        self._rebuild_basemap(force=True)

    # --- snapshot ----------------------------------------------------------------------

    def snapshot(self) -> wire.Snapshot:
        now = self.clock.now()
        alerts_src, threats_src = self.neptun.sources["alerts"], self.neptun.sources["threats"]
        alerts_stale = alerts_src.stale(now) or self.home is None
        s = wire.Snapshot(
            alert_level=wire.LEVEL_UNKNOWN if alerts_stale else regions.alert_level(self.neptun.alerts, self.home),
            alerts_stale=alerts_stale,
            threats_stale=threats_src.stale(now),
            alerts_age_s=min(0xFFFF, int(alerts_src.age(now))) if alerts_src.last_good else 0xFFFF,
            threats_age_s=min(0xFFFF, int(threats_src.age(now))) if threats_src.last_good else 0xFFFF,
            basemap_version=self.basemap.version if self.basemap else 0,
            px_per_km=self.px_per_km,
            rings_km=self.rings,
        )
        parsed = [t for t in (tracks.parse_track(item) for item in self.neptun.threats.values()) if t]
        # NEPTUN's "region" for the home oblast: the feature's "region", or "name" for Kyiv city.
        names = ((self.home.oblast_region or self.home.oblast_name),) if self.home else ()
        picture = tracks.build(parsed, self.config.home, self.config.map_radius_km, self.config.watch_radius_km,
                               now, self.home.oblast_key if self.home else None, names)
        s.threats_in_watch = picture.threats_in_watch
        s.area_only_in_region = picture.area_only_in_region if self.home else 0
        s.markers = picture.markers or []
        if picture.nearest is not None:
            s.nearest_km = picture.nearest_km
            s.nearest_kind = picture.nearest.kind
            s.nearest_heading = picture.nearest.heading
            s.nearest_advisory = picture.nearest.advisory
            s.nearest_stale = picture.nearest.stale
        return s

    # --- HTTP --------------------------------------------------------------------------

    @staticmethod
    def _nonce(request: web.Request) -> int:
        text = request.query.get("n", "")
        if len(text) != 8 or any(c not in "0123456789abcdefABCDEF" for c in text):
            raise web.HTTPBadRequest(text="n must be 8 hex digits\n")
        return int(text, 16)

    async def handle_snapshot(self, request: web.Request) -> web.Response:
        body = wire.encode_snapshot(self.snapshot(), self._nonce(request), self.config.hmac_key)
        return web.Response(body=body, content_type="application/octet-stream")

    async def handle_basemap(self, request: web.Request) -> web.Response:
        nonce = self._nonce(request)
        if self.basemap is None:
            raise web.HTTPNotFound(text="no basemap yet\n")
        return web.Response(body=self.basemap.message(nonce, self.config.hmac_key),
                            content_type="application/octet-stream")

    async def handle_test_time(self, request: web.Request) -> web.Response:
        try:
            self.clock.set(float(request.query["unix"]))
        except (KeyError, ValueError):
            raise web.HTTPBadRequest(text="unix=<seconds>\n") from None
        if self.home is None:
            if await self.neptun.fetch_regions(self.config.cache_dir):
                self.regions_loaded()
        # Fetched here rather than in the background, so every scenario frame has the same layers.
        await self._overpass()
        await self.neptun.refresh_now()
        self._rebuild_basemap(force=False)
        return web.Response(text="ok\n")

    def app(self) -> web.Application:
        application = web.Application()
        application.router.add_get("/v1/snapshot", self.handle_snapshot)
        application.router.add_get("/v1/basemap", self.handle_basemap)
        if self.test_mode:
            application.router.add_get("/_test/time", self.handle_test_time)
        return application

    async def run(self, port: int | None) -> None:
        runner = web.AppRunner(self.app(), access_log=None)
        await runner.setup()
        site = web.TCPSite(runner, self.config.listen, port if port is not None else self.config.port)
        await site.start()
        log.info("listening on %s:%s", self.config.listen, port if port is not None else self.config.port)
        tasks = [self.neptun.run(self.regions_loaded), self._watch_fills()]
        if not self.test_mode:
            tasks.append(self._overpass())
        await asyncio.gather(*tasks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="alert_gateway", description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("/etc/alert-gateway.toml"))
    parser.add_argument("--port", type=int, help="overrides the config's port")
    parser.add_argument("--test", action="store_true", help="enable /_test/time (scenario runs only)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        config = config_module.load(args.config)
    except config_module.ConfigError as error:
        log.error("%s", error)
        return 2
    try:
        asyncio.run(Gateway(config, args.test).run(args.port))
    except KeyboardInterrupt:
        pass
    except SystemExit as exit_:
        return int(exit_.code or 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())

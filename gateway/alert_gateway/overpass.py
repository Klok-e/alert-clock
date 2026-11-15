"""Water, roads and place names around home from Overpass, cached per map fingerprint."""

from __future__ import annotations

import asyncio
import json
import logging
import math
from pathlib import Path

import aiohttp

from .config import Config
from .geo import KM_PER_DEG

log = logging.getLogger(__name__)

RETRY_MIN_S = 60.0
RETRY_MAX_S = 3600.0


def bbox(config: Config) -> tuple[float, float, float, float]:
    """South, west, north, east: the screen's square plus a margin."""
    half_km = config.map_radius_km * 1.1
    dlat = half_km / KM_PER_DEG
    dlon = half_km / (KM_PER_DEG * max(0.1, math.cos(math.radians(config.home_lat))))
    return (config.home_lat - dlat, config.home_lon - dlon, config.home_lat + dlat, config.home_lon + dlon)


def query(config: Config) -> str:
    s, w, n, e = bbox(config)
    area = f"({s:.5f},{w:.5f},{n:.5f},{e:.5f})"
    return (f"[out:json][timeout:90];("
            f'way["natural"="water"]{area};relation["natural"="water"]{area};'
            f'way["waterway"="river"]{area};'
            f'way["highway"~"^(motorway|trunk|primary)$"]{area};'
            f'node["place"~"^(city|town)$"]{area};'
            f");out geom;")


def cache_path(config: Config) -> Path:
    return config.cache_dir / f"osm-{config.map_fingerprint()}.json"


def load_cached(config: Config) -> dict | None:
    try:
        return json.loads(cache_path(config).read_text())
    except (OSError, ValueError):
        return None


async def fetch_until_cached(config: Config) -> dict:
    """Queries Overpass until it answers, backing off; stores the answer in the cache."""
    delay = RETRY_MIN_S
    headers = {"User-Agent": config.user_agent}
    async with aiohttp.ClientSession(headers=headers) as session:
        while True:
            try:
                async with session.post(config.overpass_url, data={"data": query(config)},
                                        timeout=aiohttp.ClientTimeout(total=120)) as response:
                    response.raise_for_status()
                    body = json.loads(await response.read())
                if not isinstance(body, dict) or not isinstance(body.get("elements"), list):
                    raise ValueError("no elements")
                path = cache_path(config)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(body, ensure_ascii=False))
                return body
            except (aiohttp.ClientError, asyncio.TimeoutError, ValueError, OSError) as error:
                log.warning("Overpass: %s; retrying in %.0f s", error, delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, RETRY_MAX_S)

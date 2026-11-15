"""Settings from /etc/alert-gateway.toml (example: gateway/alert-gateway.example.toml)."""

from __future__ import annotations

import hashlib
import tomllib
from dataclasses import dataclass
from pathlib import Path


class ConfigError(Exception):
    """A setting the gateway cannot run with; the service logs it and exits."""


@dataclass(frozen=True)
class Config:
    home_lat: float
    home_lon: float
    map_radius_km: float
    watch_radius_km: float
    listen: str
    port: int
    hmac_key: bytes
    cache_dir: Path
    label_font: Path
    neptun_url: str
    neptun_stream: str
    overpass_url: str
    user_agent: str

    @property
    def home(self) -> tuple[float, float]:
        return (self.home_lat, self.home_lon)

    def map_fingerprint(self) -> str:
        """Identifies the Overpass area: home and map radius."""
        text = f"{self.home_lat:.5f},{self.home_lon:.5f},{self.map_radius_km:.2f}"
        return hashlib.sha256(text.encode()).hexdigest()[:16]


def load(path: Path) -> Config:
    try:
        raw = tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ConfigError(f"cannot read {path}: {error}") from error
    try:
        home, map_, gateway, upstream = raw["home"], raw["map"], raw["gateway"], raw.get("upstream", {})
        key_hex = gateway["hmac_key"]
        key = bytes.fromhex(key_hex)
        if len(key) != 32:
            raise ValueError("hmac_key must be 64 hex characters")
        config = Config(
            home_lat=float(home["lat"]),
            home_lon=float(home["lon"]),
            map_radius_km=float(map_.get("radius_km", 30)),
            watch_radius_km=float(map_.get("watch_radius_km", 25)),
            listen=str(gateway.get("listen", "0.0.0.0")),
            port=int(gateway.get("port", 8080)),
            hmac_key=key,
            cache_dir=Path(gateway.get("cache_dir", "/var/cache/alert-gateway")),
            label_font=Path(gateway.get("label_font", "/usr/share/fonts/opentype/inter/Inter-Regular.otf")),
            neptun_url=str(upstream.get("neptun", "https://neptun.in.ua")).rstrip("/"),
            neptun_stream=str(upstream.get("neptun_stream", "wss://neptun.in.ua/api/v1/stream")),
            overpass_url=str(upstream.get("overpass", "https://overpass-api.de/api/interpreter")),
            user_agent=str(upstream.get("user_agent", "alert-gateway (alert-clock)")),
        )
    except (KeyError, ValueError, TypeError) as error:
        raise ConfigError(f"bad setting in {path}: {error}") from error
    if not (-90 <= config.home_lat <= 90 and -180 <= config.home_lon <= 180):
        raise ConfigError("home coordinates out of range")
    if not (1 <= config.map_radius_km <= 200 and 0 < config.watch_radius_km <= config.map_radius_km * 1.5):
        raise ConfigError("map radius must be 1-200 km; watch radius at most 1.5 x map radius")
    if not config.label_font.is_file():
        raise ConfigError(f"label_font {config.label_font} not found (install fonts-inter)")
    return config

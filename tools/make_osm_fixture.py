#!/usr/bin/env python3
"""Shrinks a saved Overpass response (the gateway's query) into fixtures/osm/<name>.json.gz for
gateway/tests/test_basemap.py: only the tags the basemap reads, and coordinates rounded to
0.001 degrees (about 100 m, under half a pixel at a 30 km map radius).

    python3 tools/make_osm_fixture.py response.json kyiv-30km

The data is © OpenStreetMap contributors, under the ODbL (fixtures/osm/README.md).
"""

import gzip
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAGS = {"natural", "waterway", "highway", "place", "name", "name:uk", "population"}


def line(geometry):
    points = []
    for p in geometry or []:
        if "lat" in p and "lon" in p:
            point = [round(p["lat"], 3), round(p["lon"], 3)]
            if not points or points[-1] != point:
                points.append(point)
    return points


def shrink(response):
    elements = []
    for e in response["elements"]:
        item = {"type": e["type"], "tags": {k: v for k, v in e.get("tags", {}).items() if k in TAGS}}
        if e["type"] == "node":
            item["at"] = [round(e["lat"], 3), round(e["lon"], 3)]
        elif e["type"] == "way":
            item["line"] = line(e.get("geometry"))
        else:
            item["members"] = [{"role": m.get("role", ""), "line": line(m.get("geometry"))}
                               for m in e.get("members", []) if m.get("type") == "way"]
        elements.append(item)
    return {"elements": elements}


def expand(fixture):
    """Back into the Overpass `out geom` shape the gateway reads."""
    def geometry(points):
        return [{"lat": lat, "lon": lon} for lat, lon in points]
    elements = []
    for e in fixture["elements"]:
        item = {"type": e["type"], "tags": e["tags"]}
        if e["type"] == "node":
            item["lat"], item["lon"] = e["at"]
        elif e["type"] == "way":
            item["geometry"] = geometry(e["line"])
        else:
            item["members"] = [{"type": "way", "role": m["role"], "geometry": geometry(m["line"])}
                               for m in e["members"]]
        elements.append(item)
    return {"elements": elements}


def load(name):
    with gzip.open(ROOT / "fixtures" / "osm" / f"{name}.json.gz", "rt", encoding="utf-8") as f:
        return expand(json.load(f))


def main():
    source, name = sys.argv[1], sys.argv[2]
    data = json.dumps(shrink(json.loads(pathlib.Path(source).read_text())), ensure_ascii=False,
                      separators=(",", ":")).encode()
    out = ROOT / "fixtures" / "osm" / f"{name}.json.gz"
    out.write_bytes(gzip.compress(data, 9, mtime=0))
    print(f"wrote {out.relative_to(ROOT)} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()

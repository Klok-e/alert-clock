#!/usr/bin/env python3
"""Builds the PC program and runs scenarios through the whole path:
mock NEPTUN and Overpass -> the real gateway -> the clock's code -> PNG frames.

    python3 tools/run_scenarios.py                 # build, run every scenario
    python3 tools/run_scenarios.py --no-build quiet_day

Each scenario gets a fresh gateway (its config written from the scenario's "home",
"map_radius_km" and "watch_radius_km") started with --test on a free port. The scenario
program sets the gateway's clock at every step. Each scenario writes
out/scenarios/<name>/NN-<label>.png (one per step), states.jsonl and gateway.log.
Exit status 0 means every expectation in every scenario held.
"""

import argparse
import json
import os
import pathlib
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import mock_server

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENARIOS = ROOT / "fixtures" / "scenarios"
PROGRAM = ROOT / ".pio" / "build" / "native" / "program"
GATEWAY_DIR = ROOT / "gateway"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def label_font():
    """Inter Regular, as the Pi's fonts-inter package provides it."""
    try:
        found = subprocess.run(["fc-match", "-f", "%{file}", "Inter:style=Regular"], capture_output=True,
                               text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        found = ""
    if "inter" not in found.lower():
        sys.exit("Inter is not installed (fc-match Inter found none); the gateway needs it for map labels")
    return found


def write_config(path, scenario, key_hex, port, mock_base, cache_dir, font):
    home = scenario.get("home", list(mock_server.HOME))
    stream = mock_base.replace("http://", "ws://", 1) + "/api/v1/stream"
    path.write_text(f"""[home]
lat = {home[0]}
lon = {home[1]}

[map]
radius_km = {scenario.get("map_radius_km", 30)}
watch_radius_km = {scenario.get("watch_radius_km", 25)}

[gateway]
listen = "127.0.0.1"
port = {port}
hmac_key = "{key_hex}"
cache_dir = "{cache_dir}"
label_font = "{font}"

[upstream]
neptun = "{mock_base}"
neptun_stream = "{stream}"
overpass = "{mock_base}/api/interpreter"
user_agent = "alert-gateway scenario run"
""")


def wait_for_port(port, process, timeout_s=15):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def run_one(scenario_path, out, mock_base, font, env):
    scenario = json.loads(scenario_path.read_text())
    key_hex = secrets.token_hex(32)
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="alert-gateway-") as work:
        work = pathlib.Path(work)
        config = work / "gateway.toml"
        write_config(config, scenario, key_hex, port, mock_base, work / "cache", font)
        with open(out / "gateway.log", "w") as log:
            gateway = subprocess.Popen([sys.executable, "-m", "alert_gateway", "--config", str(config), "--test"],
                                       cwd=GATEWAY_DIR, stdout=log, stderr=subprocess.STDOUT)
            try:
                if not wait_for_port(port, gateway):
                    print(f"gateway did not start; see {out / 'gateway.log'}")
                    return False
                result = subprocess.run([str(PROGRAM), "--scenario", str(scenario_path),
                                         "--gateway", f"127.0.0.1:{port}", "--key", key_hex,
                                         "--mock", mock_base, "--out", str(out)], env=env)
                return result.returncode == 0
            finally:
                gateway.terminate()
                try:
                    gateway.wait(5)
                except subprocess.TimeoutExpired:
                    gateway.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="scenario names (default: all)")
    parser.add_argument("--no-build", action="store_true", help="skip `pio run -e native`")
    parser.add_argument("--out", default=str(ROOT / "out" / "scenarios"))
    args = parser.parse_args()

    if not args.no_build:
        subprocess.run(["pio", "run", "-e", "native", "--silent"], cwd=ROOT, check=True)

    files = sorted(SCENARIOS.glob("*.json"))
    if args.names:
        files = [SCENARIOS / f"{name}.json" for name in args.names]
    missing = [str(f) for f in files if not f.exists()]
    if missing or not files:
        sys.exit(f"no such scenario: {', '.join(missing) or SCENARIOS}")
    font = label_font()

    mock = mock_server.Server()
    failed = []
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    try:
        for scenario in files:
            out = pathlib.Path(args.out) / scenario.stem
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            print(f"== {scenario.stem}", flush=True)
            if not run_one(scenario, out, mock.base_url, font, env):
                failed.append(scenario.stem)
    finally:
        mock.stop()

    print(f"\n{len(files) - len(failed)}/{len(files)} scenarios passed; frames in {args.out}")
    if failed:
        print("failed: " + ", ".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()

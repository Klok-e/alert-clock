#!/usr/bin/env python3
"""Builds the PC program, starts the mock server and runs scenarios.

    python3 tools/run_scenarios.py                 # build, run every scenario
    python3 tools/run_scenarios.py --no-build quiet_day

Each scenario writes out/scenarios/<name>/NN-<label>.png (one per step) and states.jsonl.
Exit status 0 means every expectation in every scenario held.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import threading

import mock_server

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENARIOS = ROOT / "fixtures" / "scenarios"
PROGRAM = ROOT / ".pio" / "build" / "native" / "program"


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

    server = mock_server.make_server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"

    failed = []
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    for scenario in files:
        out = pathlib.Path(args.out) / scenario.stem
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        print(f"== {scenario.stem}", flush=True)
        result = subprocess.run([str(PROGRAM), "--scenario", str(scenario), "--base", base, "--out", str(out)],
                                env=env)
        if result.returncode != 0:
            failed.append(scenario.stem)
    server.shutdown()

    print(f"\n{len(files) - len(failed)}/{len(files)} scenarios passed; frames in {args.out}")
    if failed:
        print("failed: " + ", ".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Drive a reproducible CurvedStereoGS stereo benchmark matrix.

Runs the renderer once per (scene, output mode) pair, captures a per-frame CSV
trace for every run, and finally renders a Markdown report with the same
statistics the engine logs (via tools/analyze_stereo_benchmark.py).

The matrix is intentionally explicit: scene file, output mode, window size and
frame count are all recorded next to the traces, because timings are only
comparable when the GPU, the scene and the window size are identical.

Example
-------
    python tools/benchmark_matrix.py ^
        --renderer _bin/Release/CurvedStereoGS.exe ^
        --scenes models/bicycle.ply models/flower.ply ^
        --modes 2 --frames 300 --size 1920 1080 --output artifacts
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import os
import subprocess
import sys
from typing import List, Sequence

# The analyser lives next to this script; import it without requiring the tools
# directory to be a package.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_stereo_benchmark as analyzer  # noqa: E402  (import after sys.path tweak)

#: Output mode -> short label used in the generated file names.
MODE_LABELS = {0: "auto", 1: "quadbuffer", 2: "sidebyside"}


def renderer_command(args, scene: str, mode: int, csv_path: str) -> List[str]:
    command = [
        args.renderer,
        "--scene", os.path.abspath(scene),
        "--size", str(args.size[0]), str(args.size[1]),
        "--benchmark", "1",
        "--stereoOutputMode", str(mode),
        "--activeStereoGpuInterop", "1",
        "--stereoStatsCsv", os.path.abspath(csv_path),
        "--stereoStatsInterval", "0",
        "--headlessFrameCount", str(args.frames),
    ]
    if args.extra:
        command.extend(args.extra)
    return command


def run_case(args, scene: str, mode: int) -> int:
    scene_stem = os.path.splitext(os.path.basename(scene))[0]
    label = "%s_%s_%df" % (scene_stem, MODE_LABELS.get(mode, "mode%d" % mode), args.frames)
    csv_path = os.path.join(args.output, label + ".csv")
    log_path = os.path.join(args.output, label + ".log")

    command = renderer_command(args, scene, mode, csv_path)
    print("[run] %s" % " ".join(command))
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        try:
            completed = subprocess.run(
                command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout, check=False
            )
        except subprocess.TimeoutExpired:
            print("  -> timed out after %ds" % args.timeout, file=sys.stderr)
            return 124
    print("  -> exit=%d, trace=%s" % (completed.returncode, csv_path))
    return completed.returncode


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--renderer", default=os.path.join("_bin", "Release", "CurvedStereoGS.exe"))
    parser.add_argument("--scenes", nargs="+", required=True, help="one or more .ply/.spz scenes")
    parser.add_argument("--modes", nargs="+", type=int, default=[2], help="0=auto, 1=quad-buffer, 2=side-by-side")
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--size", nargs=2, type=int, default=[1920, 1080])
    parser.add_argument("--output", default="artifacts")
    parser.add_argument("--timeout", type=int, default=600, help="per-run timeout in seconds")
    parser.add_argument("--extra", nargs=argparse.REMAINDER, help="extra renderer arguments appended verbatim")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.renderer):
        print("error: renderer not found: %s" % args.renderer, file=sys.stderr)
        return 2
    for scene in args.scenes:
        if not os.path.isfile(scene):
            print("error: scene not found: %s" % scene, file=sys.stderr)
            return 2

    os.makedirs(args.output, exist_ok=True)
    started = _datetime.datetime.now()

    exits = []
    for scene in args.scenes:
        for mode in args.modes:
            exits.append(run_case(args, scene, mode))

    captures = sorted(
        os.path.join(args.output, name)
        for name in os.listdir(args.output)
        if name.endswith(".csv")
    )
    report_title = "CurvedStereoGS benchmark matrix (%s)" % started.strftime("%Y-%m-%d %H:%M")
    notes = [
        "Machine: %s, scenes: %s, window size: %dx%d, frames per run: %d."
        % (os.environ.get("COMPUTERNAME", "unknown"), ", ".join(os.path.basename(s) for s in args.scenes),
           args.size[0], args.size[1], args.frames)
    ]
    loaded = analyzer.load_captures(captures)
    report = analyzer.build_markdown(loaded, report_title, notes)
    report_path = os.path.join(args.output, "report.md")
    with open(report_path, "w", encoding="utf-8") as handle:
        handle.write(report)

    print("\n%s" % report)
    print("Report written to %s" % report_path)

    failed = [code for code in exits if code != 0]
    if failed:
        print("warning: %d of %d runs exited with a non-zero code: %s" % (len(failed), len(exits), failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

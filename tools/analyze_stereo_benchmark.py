#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Summarise CurvedStereoGS stereo-presentation frame traces.

The renderer writes one row per submitted frame when it is started with
``--stereoStatsCsv``:

    frame,path,present_ms,swap_buffers_ms

``path`` is one of ``interop`` (Vulkan exportable images + binary semaphores
consumed by the WGL quad-buffer window), ``readback`` (Vulkan readback -> GL
upload) or ``side-by-side`` (Multiview layers blitted into one swapchain image).

This tool turns those per-frame traces (and/or the ``Stereo frame statistics``
summary the renderer logs on exit) into a Markdown/JSON report with
count/mean/min/p50/p95/p99/max per presentation path.

Each capture file is reported separately: traces from different runs may come
from different GPUs, scenes or window sizes, so merging them would silently
produce meaningless averages. Paths are only compared *within* one capture.

The percentile convention is identical to ``src/stereo_frame_stats.cpp``
(nearest-rank on the retained sample window) so the report and the engine log
never disagree.

Examples
--------
    python tools/analyze_stereo_benchmark.py _bench/*.csv
    python tools/analyze_stereo_benchmark.py _bench/*.csv --markdown docs/performance-results.md
    python tools/analyze_stereo_benchmark.py _bench/run_sidebyside.log
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence

CSV_HEADER = ("frame", "path", "present_ms", "swap_buffers_ms")

#: Below this many samples the percentiles are not statistically meaningful.
MIN_MEANINGFUL_SAMPLES = 30

#: Regex for the summary lines emitted by StereoFrameStats::logSummary().
LOG_SUMMARY_RE = re.compile(
    r"^\s*(?P<path>[A-Za-z][A-Za-z0-9_\-]*)\s+"
    r"frames=(?P<frames>\d+)\s+"
    r"mean=(?P<mean>[-\d.]+)ms\s+"
    r"p50=(?P<p50>[-\d.]+)ms\s+"
    r"p95=(?P<p95>[-\d.]+)ms\s+"
    r"p99=(?P<p99>[-\d.]+)ms\s+"
    r"max=(?P<max>[-\d.]+)ms\s*$"
)
LOG_HEADER_RE = re.compile(r"^Stereo frame statistics \((?P<context>[^)]*)\)")


class BenchmarkFormatError(ValueError):
    """Raised when an input file is not a CurvedStereoGS frame trace."""


@dataclass
class FrameSample:
    frame: int
    present_ms: float
    swap_buffers_ms: float


@dataclass
class Trace:
    """All samples of a single presentation path.

    ``samples`` is populated from a per-frame CSV. When the data comes from the
    renderer's own log summary only the aggregate is available, which is stored
    in ``summary`` instead.
    """

    name: str
    samples: List[FrameSample] = field(default_factory=list)
    summary: Optional["Summary"] = None

    def present_values(self) -> List[float]:
        return [s.present_ms for s in self.samples]

    def swap_values(self) -> List[float]:
        return [s.swap_buffers_ms for s in self.samples]

    def present_summary(self) -> "Summary":
        if self.summary is not None:
            return self.summary
        return summarize(self.present_values())


@dataclass
class Capture:
    """One trace file: a set of paths measured under identical settings."""

    source: str
    traces: Dict[str, Trace] = field(default_factory=dict)

    def frame_count(self) -> int:
        return sum(trace.present_summary().count for trace in self.traces.values())


@dataclass
class Summary:
    count: int
    mean: float
    minimum: float
    p50: float
    p95: float
    p99: float
    maximum: float

    def as_dict(self) -> Dict[str, float]:
        return {
            "count": self.count,
            "mean": self.mean,
            "min": self.minimum,
            "p50": self.p50,
            "p95": self.p95,
            "p99": self.p99,
            "max": self.maximum,
        }


def percentile(sorted_values: Sequence[float], p: float) -> float:
    """Nearest-rank percentile, matching src/stereo_frame_stats.cpp."""
    if not sorted_values:
        return 0.0
    index = min(int((len(sorted_values) - 1) * p + 0.5), len(sorted_values) - 1)
    return sorted_values[index]


def summarize(values: Iterable[float]) -> Summary:
    data = sorted(values)
    if not data:
        return Summary(0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
    return Summary(
        count=len(data),
        mean=sum(data) / len(data),
        minimum=data[0],
        p50=percentile(data, 0.50),
        p95=percentile(data, 0.95),
        p99=percentile(data, 0.99),
        maximum=data[-1],
    )


def _normalise_path(name: str) -> str:
    return name.strip().lower().replace("_", "-")


def parse_csv_file(path: str) -> Capture:
    """Read a ``--stereoStatsCsv`` trace into a Capture."""
    capture = Capture(source=path)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)
        try:
            header = tuple(cell.strip() for cell in next(reader))
        except StopIteration:
            return capture
        if header[:3] != CSV_HEADER[:3]:
            raise BenchmarkFormatError(
                "%s: expected header %s, found %s" % (path, ",".join(CSV_HEADER), ",".join(header))
            )
        for lineno, row in enumerate(reader, start=2):
            if not row or not any(cell.strip() for cell in row):
                continue
            if len(row) < 3:
                raise BenchmarkFormatError("%s:%d: truncated row %r" % (path, lineno, row))
            name = _normalise_path(row[1])
            try:
                frame = int(row[0])
                present_ms = float(row[2])
                swap_ms = float(row[3]) if len(row) > 3 and row[3].strip() else 0.0
            except ValueError as exc:  # pragma: no cover - defensive
                raise BenchmarkFormatError("%s:%d: %s" % (path, lineno, exc)) from exc
            trace = capture.traces.setdefault(name, Trace(name=name))
            trace.samples.append(FrameSample(frame, present_ms, swap_ms))
    return capture


def parse_log_file(path: str) -> Dict[str, Summary]:
    """Read the ``Stereo frame statistics`` block(s) the renderer logs on exit.

    A log only carries the engine's own summary (one line per path), so ``min``
    is unavailable and reported as ``nan``.
    """
    summaries: Dict[str, Summary] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    for index, line in enumerate(lines):
        if not LOG_HEADER_RE.match(line.strip()):
            continue
        for candidate in lines[index + 1 :]:
            match = LOG_SUMMARY_RE.match(candidate.rstrip("\n"))
            if match is None:
                break
            summaries[_normalise_path(match.group("path"))] = Summary(
                count=int(match.group("frames")),
                mean=float(match.group("mean")),
                minimum=float("nan"),
                p50=float(match.group("p50")),
                p95=float(match.group("p95")),
                p99=float(match.group("p99")),
                maximum=float(match.group("max")),
            )
    return summaries


def _format_speedup(slow_ms: float, fast_ms: float) -> str:
    if fast_ms <= 0.0:
        return "n/a"
    return "%.2fx" % (slow_ms / fast_ms)


def load_captures(paths: Sequence[str]) -> List[Capture]:
    captures: List[Capture] = []
    for path in paths:
        if path.lower().endswith(".csv"):
            captures.append(parse_csv_file(path))
        elif path.lower().endswith((".log", ".txt")):
            summaries = parse_log_file(path)
            if not summaries:
                continue
            capture = Capture(source="%s (log summary)" % path)
            for name, summary in sorted(summaries.items()):
                capture.traces[name] = Trace(name=name, summary=summary)
            captures.append(capture)
        else:
            raise BenchmarkFormatError("%s: expected a .csv or .log file" % path)
    return captures


def _render_path_table(lines: List[str], capture: Capture) -> None:
    lines.append("| path | frames | mean (ms) | min (ms) | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for name in sorted(capture.traces):
        stats = capture.traces[name].present_summary()
        minimum = "n/a" if stats.minimum != stats.minimum else "%.3f" % stats.minimum
        lines.append(
            "| `%s` | %d | %.3f | %s | %.3f | %.3f | %.3f | %.3f |"
            % (name, stats.count, stats.mean, minimum, stats.p50, stats.p95, stats.p99, stats.maximum)
        )


def _render_comparison(lines: List[str], capture: Capture) -> None:
    measurable = {
        name: capture.traces[name].present_summary().mean
        for name in capture.traces
        if capture.traces[name].present_summary().count >= MIN_MEANINGFUL_SAMPLES
    }
    if len(measurable) < 2:
        lines.append("Fewer than two paths have enough samples for a comparison.")
        return
    ordered = sorted(measurable, key=lambda name: measurable[name])
    baseline = ordered[-1]
    lines.append("Ranked by mean CPU cost per frame (fastest first):")
    lines.append("")
    lines.append("| rank | path | mean (ms) | vs `%s` |" % baseline)
    lines.append("| ---: | --- | ---: | ---: |")
    for rank, name in enumerate(ordered, start=1):
        lines.append(
            "| %d | `%s` | %.3f | %s |"
            % (rank, name, measurable[name], _format_speedup(measurable[baseline], measurable[name]))
        )


def build_markdown(captures: Sequence[Capture], title: str, notes: Sequence[str] = ()) -> str:
    lines: List[str] = ["# %s" % title, ""]

    if not captures:
        lines.append("No frame samples were found in the supplied capture files.")
        lines.append("")
        return "\n".join(lines)

    lines.append(
        "%d capture(s), %d frame samples in total. Each capture is reported on its own:"
        % (len(captures), sum(capture.frame_count() for capture in captures))
    )
    lines.append("traces from different runs must not be averaged together.")
    lines.append("")

    for capture in captures:
        lines.append("## `%s`" % os.path.basename(capture.source))
        lines.append("")
        if not capture.traces:
            lines.append("_No rows in this capture._")
            lines.append("")
            continue
        lines.append("### Per-path CPU presentation cost")
        lines.append("")
        _render_path_table(lines, capture)
        lines.append("")

        swap_paths = [name for name in sorted(capture.traces) if any(v > 0.0 for v in capture.traces[name].swap_values())]
        if swap_paths:
            lines.append("### SwapBuffers sub-metric")
            lines.append("")
            lines.append("| path | frames | mean (ms) | p95 (ms) | p99 (ms) | max (ms) |")
            lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
            for name in swap_paths:
                stats = summarize(capture.traces[name].swap_values())
                lines.append(
                    "| `%s` | %d | %.3f | %.3f | %.3f | %.3f |"
                    % (name, stats.count, stats.mean, stats.p95, stats.p99, stats.maximum)
                )
            lines.append("")

        lines.append("### Path comparison")
        lines.append("")
        _render_comparison(lines, capture)
        lines.append("")

        thin = sorted(
            name for name in capture.traces if capture.traces[name].present_summary().count < MIN_MEANINGFUL_SAMPLES
        )
        if thin:
            lines.append(
                "> **Not statistically meaningful:** %s have fewer than %d samples; they are excluded"
                % (", ".join("`%s`" % name for name in thin), MIN_MEANINGFUL_SAMPLES)
            )
            lines.append("> from the comparison above.")
            lines.append("")

    for note in notes:
        lines.append("> %s" % note)
    if notes:
        lines.append("")

    lines.append("<details>")
    lines.append("<summary>Capture sources</summary>")
    lines.append("")
    for capture in captures:
        lines.append("- `%s`" % capture.source)
    lines.append("")
    lines.append("</details>")
    lines.append("")
    return "\n".join(lines)


def build_json(captures: Sequence[Capture]) -> Dict[str, object]:
    return {
        "captures": [
            {
                "source": capture.source,
                "paths": {
                    name: {
                        "present_ms": capture.traces[name].present_summary().as_dict(),
                        "swap_buffers_ms": summarize(capture.traces[name].swap_values()).as_dict(),
                    }
                    for name in sorted(capture.traces)
                },
            }
            for capture in captures
        ]
    }


def expand_arguments(arguments: Sequence[str]) -> List[str]:
    expanded: List[str] = []
    for argument in arguments:
        matches = sorted(glob.glob(argument))
        expanded.extend(matches if matches else [argument])
    return expanded


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", nargs="+", help="per-frame CSV traces (globs allowed) and/or renderer logs")
    parser.add_argument("--title", default="CurvedStereoGS stereo presentation timings")
    parser.add_argument("--markdown", help="write the Markdown report to this file")
    parser.add_argument("--json", dest="json_path", help="write the summary as JSON to this file")
    parser.add_argument("--note", action="append", default=[], help="extra caveat appended to the report")
    args = parser.parse_args(argv)

    paths = expand_arguments(args.captures)
    existing = [path for path in paths if os.path.exists(path)]
    for path in paths:
        if path not in existing:
            print("warning: %s does not exist" % path, file=sys.stderr)

    captures = load_captures(existing)
    report = build_markdown(captures, args.title, args.note)
    print(report)

    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(report)
        print("Markdown report written to %s" % args.markdown, file=sys.stderr)

    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump(build_json(captures), handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("JSON summary written to %s" % args.json_path, file=sys.stderr)

    if not any(capture.traces for capture in captures):
        print("error: no frame samples found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

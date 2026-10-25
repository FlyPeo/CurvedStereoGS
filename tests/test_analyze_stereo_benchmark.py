# -*- coding: utf-8 -*-
"""Unit tests for tools/analyze_stereo_benchmark.py.

The analyser is the only component of the stereo tool-chain that can be tested
without a GPU, so it is covered here and wired into CTest as
``benchmark_report_tool``. Run it directly with::

    python -m unittest discover -s tests -p "test_*.py"
"""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL_PATH = os.path.join(REPO_ROOT, "tools", "analyze_stereo_benchmark.py")


def _load_tool():
    spec = importlib.util.spec_from_file_location("analyze_stereo_benchmark", TOOL_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tool = _load_tool()


CSV_SAMPLE = """\
frame,path,present_ms,swap_buffers_ms
1,side-by-side,0.010,0.000
2,side-by-side,0.030,1.500
3,interop,0.020,0.000
"""


class PercentileTests(unittest.TestCase):
    def test_nearest_rank_matches_engine_convention(self):
        # src/stereo_frame_stats.cpp: index = min(int((n - 1) * p + 0.5), n - 1)
        values = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]
        self.assertEqual(tool.percentile(values, 0.0), 1.0)
        self.assertEqual(tool.percentile(values, 0.50), 6.0)
        self.assertEqual(tool.percentile(values, 0.95), 10.0)
        self.assertEqual(tool.percentile(values, 1.0), 10.0)

    def test_percentile_of_empty_sequence_is_zero(self):
        self.assertEqual(tool.percentile([], 0.5), 0.0)

    def test_summarize_reports_all_expected_fields(self):
        summary = tool.summarize([4.0, 1.0, 3.0, 2.0])
        self.assertEqual(summary.count, 4)
        self.assertAlmostEqual(summary.mean, 2.5)
        self.assertEqual(summary.minimum, 1.0)
        self.assertEqual(summary.maximum, 4.0)
        self.assertEqual(summary.p50, 3.0)  # sorted -> [1,2,3,4], idx round(3*0.5)=2

    def test_summarize_of_empty_sequence_is_all_zero(self):
        summary = tool.summarize([])
        self.assertEqual(summary.as_dict(), {"count": 0, "mean": 0.0, "min": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0})


class CsvParsingTests(unittest.TestCase):
    def _write(self, text: str) -> str:
        handle = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def test_parses_rows_and_groups_by_path(self):
        capture = tool.parse_csv_file(self._write(CSV_SAMPLE))
        self.assertEqual(len(capture.traces), 2)
        side = capture.traces["side-by-side"]
        self.assertEqual(len(side.samples), 2)
        self.assertEqual(side.samples[1].swap_buffers_ms, 1.5)
        self.assertEqual(capture.traces["interop"].samples[0].present_ms, 0.02)
        self.assertEqual(capture.frame_count(), 3)

    def test_path_names_are_normalised(self):
        capture = tool.parse_csv_file(self._write("frame,path,present_ms,swap_buffers_ms\n1,Side_By_Side,1,0\n"))
        self.assertIn("side-by-side", capture.traces)

    def test_empty_capture_is_allowed(self):
        capture = tool.parse_csv_file(self._write(""))
        self.assertEqual(capture.frame_count(), 0)

    def test_rejects_unknown_header(self):
        with self.assertRaises(tool.BenchmarkFormatError):
            tool.parse_csv_file(self._write("frame,present_ms\n1,0.5\n"))


class LogParsingTests(unittest.TestCase):
    def test_reads_engine_summary_block(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".log", delete=False, encoding="utf-8")
        handle.write(
            "Stereo frame statistics (final): 300 frames measured\n"
            "  side-by-side frames=300  mean=0.013ms  p50=0.011ms  p95=0.019ms  p99=0.025ms  max=0.150ms\n"
            "GPU particle AS build skipped (no instances). RTX disabled.\n"
        )
        handle.close()
        self.addCleanup(os.unlink, handle.name)

        summaries = tool.parse_log_file(handle.name)
        self.assertIn("side-by-side", summaries)
        self.assertEqual(summaries["side-by-side"].count, 300)
        self.assertAlmostEqual(summaries["side-by-side"].p99, 0.025)


class MarkdownReportTests(unittest.TestCase):
    def _capture(self, samples):
        capture = tool.Capture(source="synthetic.csv")
        trace = tool.Trace(name="side-by-side")
        trace.samples = [tool.FrameSample(i, value, 0.0) for i, value in enumerate(samples)]
        capture.traces[trace.name] = trace
        return capture

    def test_report_contains_statistics_row(self):
        report = tool.build_markdown([self._capture([1.0, 2.0, 3.0])], "T")
        self.assertIn("| `side-by-side` | 3 |", report)

    def test_thin_capture_is_flagged_and_not_compared(self):
        report = tool.build_markdown([self._capture([1.0, 2.0])], "T")
        self.assertIn("Not statistically meaningful", report)
        self.assertIn("Fewer than two paths", report)

    def test_comparison_ranks_paths_within_one_capture(self):
        capture = tool.Capture(source="synthetic.csv")
        fast = tool.Trace(name="side-by-side")
        fast.samples = [tool.FrameSample(i, 0.01, 0.0) for i in range(100)]
        slow = tool.Trace(name="readback")
        slow.samples = [tool.FrameSample(i, 1.00, 0.0) for i in range(100)]
        capture.traces = {"side-by-side": fast, "readback": slow}

        report = tool.build_markdown([capture], "T")
        self.assertIn("| 1 | `side-by-side` | 0.010 | 100.00x |", report)

    def test_no_captures_message(self):
        report = tool.build_markdown([], "T")
        self.assertIn("No frame samples were found", report)


class CliTests(unittest.TestCase):
    def test_cli_writes_markdown_and_json(self):
        with tempfile.TemporaryDirectory() as directory:
            csv_path = os.path.join(directory, "trace.csv")
            with open(csv_path, "w", encoding="utf-8") as handle:
                handle.write(CSV_SAMPLE)
            md_path = os.path.join(directory, "report.md")
            json_path = os.path.join(directory, "report.json")

            exit_code = tool.main([csv_path, "--markdown", md_path, "--json", json_path])
            self.assertEqual(exit_code, 0)
            self.assertTrue(os.path.exists(md_path))
            with open(json_path, "r", encoding="utf-8") as handle:
                payload = handle.read()
            self.assertIn("side-by-side", payload)

    def test_cli_fails_without_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = os.path.join(directory, "empty.csv")
            open(empty, "w", encoding="utf-8").close()
            self.assertEqual(tool.main([empty]), 1)


if __name__ == "__main__":
    unittest.main()

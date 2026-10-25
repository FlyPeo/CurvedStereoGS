/*
 * Unit tests for the CPU stereo-presentation statistics module.
 *
 * StereoFrameStats deliberately keeps its accumulators private, so these tests
 * drive the module through its public API and verify the observable contract:
 * the per-frame CSV trace (header, one row per committed frame, path naming and
 * the SwapBuffers sub-metric) plus the ignore-after-close / mismatch behaviour.
 *
 * The tests are CPU-only and do not need a Vulkan device, so they run in CI and
 * on any developer machine. Build with -DCURVED_STEREO_GS_BUILD_TESTS=ON and
 * run through CTest (see the "Tests" section of CMakeLists.txt).
 */

#include "stereo_frame_stats.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using curved_stereo_gs::StereoFrameStats;

int g_failures = 0;
int g_checks   = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
  ++g_checks;
  if(!condition)
  {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

std::filesystem::path makeTempCsvPath(const char* stem)
{
  const auto unique = std::to_string(std::rand()) + "_" + std::to_string(std::rand());
  return std::filesystem::temp_directory_path() / (std::string(stem) + "_" + unique + ".csv");
}

struct CsvRow
{
  std::string frame;
  std::string path;
  std::string presentMs;
  std::string swapBuffersMs;
};

std::vector<CsvRow> readCsv(const std::filesystem::path& path, std::string& header)
{
  std::vector<CsvRow> rows;
  std::ifstream        stream(path);
  if(!std::getline(stream, header))
    return rows;

  std::string line;
  while(std::getline(stream, line))
  {
    if(line.empty())
      continue;
    std::istringstream     fields(line);
    std::vector<std::string> cells;
    std::string              cell;
    while(std::getline(fields, cell, ','))
      cells.push_back(cell);
    if(cells.size() >= 4)
      rows.push_back({cells[0], cells[1], cells[2], cells[3]});
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Path naming
// ---------------------------------------------------------------------------
void testPathNames()
{
  CHECK(std::string(StereoFrameStats::pathName(StereoFrameStats::Path::kGpuInterop)) == "interop");
  CHECK(std::string(StereoFrameStats::pathName(StereoFrameStats::Path::kCpuReadback)) == "readback");
  CHECK(std::string(StereoFrameStats::pathName(StereoFrameStats::Path::kVulkanSideBySide)) == "side-by-side");
  CHECK(std::string(StereoFrameStats::pathName(StereoFrameStats::Path::kPathCount)) == "unknown");
}

// ---------------------------------------------------------------------------
// CSV trace: header, row count, field naming and per-frame values
// ---------------------------------------------------------------------------
void testCsvTrace()
{
  const std::filesystem::path csvPath = makeTempCsvPath("curved_stereo_gs_stats");

  CHECK(StereoFrameStats::openCsv(csvPath.string().c_str()));
  StereoFrameStats::setReportInterval(0);  // keep the test output quiet

  constexpr uint32_t kFrames = 50;
  for(uint32_t i = 0; i < kFrames; ++i)
  {
    StereoFrameStats::beginFrame(StereoFrameStats::Path::kGpuInterop, 1000 + i);
    if(i == 7)
      StereoFrameStats::setSwapBuffersMs(1.25);
    StereoFrameStats::endFrame(StereoFrameStats::Path::kGpuInterop);
  }
  StereoFrameStats::closeCsv();

  std::string          header;
  const std::vector<CsvRow> rows = readCsv(csvPath, header);
  CHECK(header == "frame,path,present_ms,swap_buffers_ms");
  CHECK(rows.size() == kFrames);
  if(rows.size() == kFrames)
  {
    CHECK(rows.front().path == "interop");
    CHECK(rows.front().frame == "1000");
    CHECK(rows.back().frame == "1049");  // 1000 + (kFrames - 1)
    CHECK(rows.back().path == "interop");
    // A frame that never reported SwapBuffers must still emit the field.
    CHECK(rows.front().swapBuffersMs == "0.000");
    // The reported SwapBuffers sample must survive into row 7.
    CHECK(rows[7].swapBuffersMs == "1.250");
  }

  std::error_code ignored;
  std::filesystem::remove(csvPath, ignored);
}

// ---------------------------------------------------------------------------
// endFrame with a different path than beginFrame must be ignored
// ---------------------------------------------------------------------------
void testMismatchedPathDoesNotCommit()
{
  const std::filesystem::path csvPath = makeTempCsvPath("curved_stereo_gs_stats_mismatch");

  CHECK(StereoFrameStats::openCsv(csvPath.string().c_str()));
  StereoFrameStats::beginFrame(StereoFrameStats::Path::kCpuReadback, 1);
  StereoFrameStats::endFrame(StereoFrameStats::Path::kGpuInterop);  // wrong path -> ignored
  StereoFrameStats::closeCsv();

  std::string               header;
  const std::vector<CsvRow> rows = readCsv(csvPath, header);
  CHECK(rows.empty());

  std::error_code ignored;
  std::filesystem::remove(csvPath, ignored);
}

// ---------------------------------------------------------------------------
// The module must not crash and must ignore samples after the CSV is closed
// ---------------------------------------------------------------------------
void testFramesAfterCloseAreIgnored()
{
  const std::filesystem::path csvPath = makeTempCsvPath("curved_stereo_gs_stats_closed");

  CHECK(StereoFrameStats::openCsv(csvPath.string().c_str()));
  StereoFrameStats::beginFrame(StereoFrameStats::Path::kVulkanSideBySide, 1);
  StereoFrameStats::endFrame(StereoFrameStats::Path::kVulkanSideBySide);
  StereoFrameStats::closeCsv();

  // Closing twice and sampling afterwards used to be an easy way to crash.
  StereoFrameStats::closeCsv();
  StereoFrameStats::beginFrame(StereoFrameStats::Path::kVulkanSideBySide, 2);
  StereoFrameStats::endFrame(StereoFrameStats::Path::kVulkanSideBySide);

  // Opening a path whose parent directory does not exist must fail cleanly.
  const std::filesystem::path missing =
      std::filesystem::temp_directory_path() / "curved_stereo_gs_no_such_dir" / "trace.csv";
  CHECK(!StereoFrameStats::openCsv(missing.string().c_str()));

  StereoFrameStats::logSummary("unit-test");  // must be safe to call at any time

  std::string               header;
  const std::vector<CsvRow> rows = readCsv(csvPath, header);
  CHECK(rows.size() == 1);
  if(rows.size() == 1)
    CHECK(rows.front().path == "side-by-side");

  std::error_code ignored;
  std::filesystem::remove(csvPath, ignored);
  std::filesystem::remove(missing, ignored);
}

}  // namespace

int main()
{
  testPathNames();
  testCsvTrace();
  testMismatchedPathDoesNotCommit();
  testFramesAfterCloseAreIgnored();

  if(g_failures == 0)
  {
    std::printf("StereoFrameStats unit tests: %d checks passed\n", g_checks);
    return EXIT_SUCCESS;
  }

  std::fprintf(stderr, "StereoFrameStats unit tests: %d of %d checks failed\n", g_failures, g_checks);
  return EXIT_FAILURE;
}

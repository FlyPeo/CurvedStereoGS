/*
 * CPU frame-time statistics for the stereo presentation paths.
 * See stereo_frame_stats.h for the module contract.
 */

#include "stereo_frame_stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

#include <nvutils/logger.hpp>

namespace curved_stereo_gs {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSampleRingSize = 8192;

struct PathAccumulator
{
  uint64_t      count{0};
  double        totalMs{0.0};
  double        maxMs{0.0};
  std::vector<double> sampleRing{};   // last kSampleRingSize presentMs values
  uint32_t      ringWriteIndex{0};
  bool          ringFilled{false};

  void addSample(double ms)
  {
    count += 1;
    totalMs += ms;
    maxMs = std::max(maxMs, ms);
    if(sampleRing.empty())
      sampleRing.resize(kSampleRingSize, 0.0);
    sampleRing[ringWriteIndex] = ms;
    ringWriteIndex             = (ringWriteIndex + 1U) % kSampleRingSize;
    if(ringWriteIndex == 0U)
      ringFilled = true;
  }

  double meanMs() const { return count == 0 ? 0.0 : totalMs / double(count); }

  // Percentile over the retained sample window (p in [0, 1]).
  double percentileMs(double p) const
  {
    if(count == 0)
      return 0.0;
    const uint32_t retained = ringFilled ? kSampleRingSize : ringWriteIndex;
    std::vector<double> sorted(sampleRing.begin(), sampleRing.begin() + retained);
    std::sort(sorted.begin(), sorted.end());
    const size_t index =
        std::min(size_t(double(retained - 1) * p + 0.5), size_t(retained - 1));
    return sorted[index];
  }
};

struct StereoFrameStatsImpl
{
  struct OpenFrame
  {
    bool       active{false};
    StereoFrameStats::Path path{StereoFrameStats::Path::kGpuInterop};
    uint64_t   frameNumber{0};
    Clock::time_point startTime{};
    double     swapBuffersMs{0.0};
  };

  OpenFrame                              open{};
  std::array<PathAccumulator, size_t(StereoFrameStats::Path::kPathCount)> paths{};
  uint64_t                               totalFrames{0};
  uint32_t                               reportInterval{0};  // 0 = off
  std::FILE*                             csvFile{nullptr};
};

StereoFrameStatsImpl& theStats()
{
  static StereoFrameStatsImpl instance;
  return instance;
}

const char* pathDisplayName(StereoFrameStats::Path path)
{
  switch(path)
  {
    case StereoFrameStats::Path::kGpuInterop:
      return "interop";
    case StereoFrameStats::Path::kCpuReadback:
      return "readback";
    case StereoFrameStats::Path::kVulkanSideBySide:
      return "side-by-side";
    default:
      return "unknown";
  }
}

}  // namespace

const char* StereoFrameStats::pathName(Path path)
{
  return pathDisplayName(path);
}

void StereoFrameStats::beginFrame(Path path, uint64_t frameNumber)
{
  StereoFrameStatsImpl& stats = theStats();
  if(stats.open.active)
  {
    // A previous frame was never closed (early-out path); keep the partial
    // sample out of the statistics rather than mixing two paths into one.
    stats.open.active = false;
  }
  stats.open.active      = true;
  stats.open.path        = path;
  stats.open.frameNumber = frameNumber;
  stats.open.startTime   = Clock::now();
  stats.open.swapBuffersMs = 0.0;
}

void StereoFrameStats::setSwapBuffersMs(double milliseconds)
{
  StereoFrameStatsImpl& stats = theStats();
  if(stats.open.active)
    stats.open.swapBuffersMs = milliseconds;
}

void StereoFrameStats::endFrame(Path path)
{
  StereoFrameStatsImpl& stats = theStats();
  if(!stats.open.active || stats.open.path != path)
    return;
  stats.open.active = false;

  const double presentMs =
      std::chrono::duration<double, std::milli>(Clock::now() - stats.open.startTime).count();
  PathAccumulator& acc = stats.paths[size_t(path)];
  acc.addSample(presentMs);
  stats.totalFrames += 1;

  if(stats.csvFile != nullptr)
  {
    std::fprintf(stats.csvFile, "%llu,%s,%.3f,%.3f\n",
                 static_cast<unsigned long long>(stats.open.frameNumber),
                 pathDisplayName(path), presentMs, stats.open.swapBuffersMs);
    std::fflush(stats.csvFile);
  }

  if(stats.reportInterval != 0 && stats.totalFrames % stats.reportInterval == 0)
    logSummary("periodic");
}

void StereoFrameStats::setReportInterval(uint32_t interval)
{
  theStats().reportInterval = interval;
}

bool StereoFrameStats::openCsv(const char* filePath)
{
  StereoFrameStatsImpl& stats = theStats();
  closeCsv();
  stats.csvFile = std::fopen(filePath, "w");
  if(stats.csvFile == nullptr)
    return false;
  std::fprintf(stats.csvFile, "frame,path,present_ms,swap_buffers_ms\n");
  return true;
}

void StereoFrameStats::closeCsv()
{
  StereoFrameStatsImpl& stats = theStats();
  if(stats.csvFile != nullptr)
  {
    std::fclose(stats.csvFile);
    stats.csvFile = nullptr;
  }
}

void StereoFrameStats::logSummary(const char* context)
{
  StereoFrameStatsImpl& stats = theStats();
  if(stats.totalFrames == 0)
    return;

  LOGI("Stereo frame statistics (%s): %llu frames measured\n", context,
       static_cast<unsigned long long>(stats.totalFrames));
  for(size_t i = 0; i < size_t(Path::kPathCount); ++i)
  {
    const PathAccumulator& acc = stats.paths[i];
    if(acc.count == 0)
      continue;
    LOGI("  %-12s frames=%llu  mean=%.3fms  p50=%.3fms  p95=%.3fms  p99=%.3fms  max=%.3fms\n",
         pathDisplayName(Path(i)), static_cast<unsigned long long>(acc.count), acc.meanMs(),
         acc.percentileMs(0.50), acc.percentileMs(0.95), acc.percentileMs(0.99), acc.maxMs);
  }
}

}  // namespace curved_stereo_gs

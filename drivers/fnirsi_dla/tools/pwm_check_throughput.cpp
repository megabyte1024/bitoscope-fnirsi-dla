/*
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pwm_check.h"
#include "pwm_synth.h"

namespace {

namespace tools = fnirsi::tools;
namespace synth = fnirsi::tools::synth;

constexpr uint64_t kRateHz = 1'000'000'000;
constexpr uint64_t kPeriodSamples = 10'000'000;  // WHY: 10 ms holds whole periods of all four PWMs, so replayed laps join without an edge shift.
constexpr uint64_t kTargetBufferBytes = 250'000'000;
constexpr uint64_t kMilliHz[4] = {1'300'000, 2'100'000, 3'700'000, 5'300'000};
constexpr uint32_t kDuty[4] = {25, 50, 75, 40};

struct Result {
  double mb_per_s = 0.0;
  uint64_t bytes = 0;
  double seconds = 0.0;
  bool clean = false;
  std::string detail;
};

Result measure(const std::vector<uint8_t>& buffer, uint32_t n, uint32_t workers, uint64_t chunk_bytes,
               double seconds, const std::vector<tools::CheckedChannel>& checked) {
  tools::PwmCheckConfig config;
  config.num_channels = n;
  config.sample_rate_milli_hz = kRateHz * 1000;
  config.channels = checked;
  config.workers = workers;
  auto checker = tools::PwmStreamChecker::create(config);
  if (!checker) {
    Result r;
    r.detail = checker.error();
    return r;
  }
  bitoscope::bulk_ring::ChunkHeader header;
  header.num_channels = n;
  header.encoding = tools::kChannelMajorPackedEncoding;
  header.sample_rate_milli_hz = kRateHz * 1000;
  uint64_t next = 0;
  uint64_t bytes = 0;
  std::chrono::steady_clock::duration busy{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    for (size_t offset = 0; offset < buffer.size(); offset += chunk_bytes) {
      const size_t size = std::min<size_t>(chunk_bytes, buffer.size() - offset);
      header.first_sample_index = next;
      header.num_samples = size / n * 8;
      header.payload_bytes = size;
      const auto t0 = std::chrono::steady_clock::now();
      (*checker)->checkChunk(header, std::span<const uint8_t>(buffer.data() + offset, size));
      busy += std::chrono::steady_clock::now() - t0;
      next += header.num_samples;
      bytes += size;
    }
  }
  Result r;
  r.bytes = bytes;
  r.seconds = std::chrono::duration<double>(busy).count();
  r.mb_per_s = static_cast<double>(bytes) / 1e6 / r.seconds;
  const auto report = (*checker)->report();
  r.clean = report.clean();
  uint64_t edges = 0;
  for (const auto& c : report.channels) edges += c.edges;
  r.detail = std::to_string(edges) + " edges";
  if (!r.clean) {
    for (const auto& c : report.channels) {
      if (auto d = c.firstDeviation()) r.detail += "; " + tools::describe(*d, c.shape, 1, 1);
    }
    if (report.stream_error) r.detail += "; " + report.stream_error->message;
  }
  return r;
}

}

int main(int argc, char** argv) {
  double seconds = 3.0;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "usage: %s [--seconds S]\n", argv[0]);
      return 2;
    }
  }
  std::printf("PWM checker throughput: rig's signal at 1 GHz from ~250 MB buffer (cold), %u hardware "
              "threads, %.1f s per row\n",
              std::thread::hardware_concurrency(), seconds);
  std::printf("%8s %8s %10s %12s %12s %8s  %s\n", "channels", "workers", "chunk", "payload MB/s", "Gbit/s",
              "clean", "stream");
  bool all_clean = true;
  for (const uint32_t n : {2u, 4u, 8u, 16u}) {
    const uint64_t bytes_per_period = kPeriodSamples / 8 * n;
    const uint64_t periods = std::max<uint64_t>(1, kTargetBufferBytes / bytes_per_period);
    const uint64_t length = periods * kPeriodSamples;
    std::vector<synth::ChannelSignal> signals;
    std::vector<tools::CheckedChannel> checked;
    for (uint32_t c = 0; c < n; ++c) {
      if (c < 4) {
        const auto shape = tools::pwmShapeFor(kRateHz, kMilliHz[c], kDuty[c]).value();
        signals.push_back(synth::pwmSignal(shape, (c * 7919 + 13) % shape.period_num, length));
        checked.push_back(tools::CheckedChannel{c, shape});
      } else {
        signals.push_back(synth::flatSignal(false, length));
      }
    }
    const std::vector<uint8_t> buffer = synth::pack(signals);
    signals.clear();
    for (const uint64_t chunk : {uint64_t{4} << 20, uint64_t{64} << 10}) {
      for (const uint32_t workers : {0u, 1u, 2u, 4u}) {
        if (workers > checked.size()) continue;
        const Result r = measure(buffer, n, workers, chunk, seconds, checked);
        all_clean = all_clean && r.clean;
        std::printf("%8u %8u %7llu KiB %12.0f %12.1f %8s  %s\n", n, workers,
                    static_cast<unsigned long long>(chunk >> 10), r.mb_per_s, r.mb_per_s * 8 / 1000,
                    r.clean ? "yes" : "NO", r.detail.c_str());
        std::fflush(stdout);
      }
    }
  }
  std::printf("workers 0: every channel on calling thread (ring reader's). workers W: checked channels "
              "dealt over W threads, each reading every payload word its channels live in.\n");
  return all_clean ? 0 : 1;
}

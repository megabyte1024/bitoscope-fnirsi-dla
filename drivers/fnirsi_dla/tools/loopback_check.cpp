/*
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dla_session.h"
#include "fnirsi_protocol.h"
#include "ring_reader.h"
#include "sample_decode.h"
#include "sample_encoding.h"

namespace {

constexpr auto kDriverStartTimeout = std::chrono::seconds(30);
constexpr auto kCommandTimeout = std::chrono::seconds(15);
constexpr auto kSampleTimeout = std::chrono::seconds(20);
constexpr double kFrequencyTolerance = 0.05;

struct PortTest {
  fnirsi::PwmPort port;
  uint32_t channel;
  double frequency_hz;
};

const std::vector<PortTest> kPorts = {
    {fnirsi::PwmPort::kP0, 0, 1300.0},
    {fnirsi::PwmPort::kP1, 1, 2100.0},
    {fnirsi::PwmPort::kP2, 2, 3700.0},
    {fnirsi::PwmPort::kP3, 3, 5300.0},
};

bool startGenerator(bitoscope::host::pipe::SiggenClient& siggen, const PortTest& p) {
  const uint32_t mask = 1u << (static_cast<uint8_t>(p.port) - static_cast<uint8_t>(fnirsi::PwmPort::kP0));
  // WHY: Siggen START starts what was last configured, so CONFIGURE the square wave before START.
  auto result = siggen.configureSquare(mask, {static_cast<uint64_t>(p.frequency_hz * 1000), 50});
  if (result) result = siggen.start(mask, bitoscope::host::pipe::StartMode::Independent);
  if (!result) {
    std::fprintf(stderr, "  FAILED to start generator on channel %u: %s\n", p.channel, result.error().c_str());
    return false;
  }
  return true;
}

bool stopGenerator(bitoscope::host::pipe::SiggenClient& siggen, uint32_t channel_mask) {
  return siggen.stop(channel_mask).has_value();
}

std::mutex g_sample_mutex;
std::vector<std::string> g_chunks;
uint32_t g_expected_samples = 0;
uint32_t g_received_samples = 0;
std::condition_variable g_sample_cv;

void onChunk(const bitoscope::bulk_ring::ChunkHeader& header, std::span<const uint8_t> payload) {
  if (header.encoding != static_cast<uint32_t>(bitoscope::host::SampleEncoding::ChannelMajorPacked)) {
    std::fprintf(stderr,
                 "WARNING: dropping chunk with unsupported encoding %u "
                 "(this tool only decodes channel-major layout)\n",
                 header.encoding);
    return;
  }
  std::lock_guard<std::mutex> lock(g_sample_mutex);
  g_chunks.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
  g_received_samples += static_cast<uint32_t>(header.num_samples);
  if (g_received_samples >= g_expected_samples) {
    g_sample_cv.notify_all();
  }
}

using fnirsi::tools::estimateFrequencyHz;
using fnirsi::tools::extractChannelBits;

}

int main() {
  const std::filesystem::path driver_dir = BITOSCOPE_DEFAULT_DRIVER_DIR;
  std::printf("Starting %s from %s on connected DLA (timeout %llds)...\n",
              std::string(fnirsi::tools::kDlaDriver).c_str(), driver_dir.string().c_str(),
              static_cast<long long>(kDriverStartTimeout.count()));
  fnirsi::tools::DlaSessionOptions options;
  options.driver_dir = driver_dir;
  options.start_timeout = kDriverStartTimeout;
  options.request_timeout = kCommandTimeout;
  auto session = fnirsi::tools::DlaSession::open(options);
  if (!session) {
    std::fprintf(stderr, "FAIL: %s\n", session.error().c_str());
    return 1;
  }
  std::printf("Found driver '%s'\n", (*session)->driverDeviceId().c_str());
  auto& la = (*session)->la();
  auto& siggen = (*session)->siggen();

  if (!la.capabilities().bulk_ring) {
    std::fprintf(stderr, "FAIL: driver's la.acquisition facet does not write bulk ring, only "
                         "way samples travel.\n");
    return 1;
  }
  std::printf("Negotiated OK. la.acquisition writes bulk ring.\n");

  std::printf("Starting PWM generators (1300/2100/3700/5300 Hz on P0-P3)...\n");
  for (const auto& p : kPorts) {
    if (!startGenerator(siggen, p)) {
      stopGenerator(siggen, 0x0F);
      return 1;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // WHY: Capture starts only after this pause: PWM start and capture start are unsynchronized commands.
  auto reader = fnirsi::tools::RingReader::start(16, 4 * 1024 * 1024, onChunk);
  if (!reader.has_value()) {
    std::fprintf(stderr, "FAIL: %s\n", reader.error().c_str());
    stopGenerator(siggen, 0x0F);
    return 1;
  }

  constexpr uint64_t kSampleRateHz = 1'000'000;
  constexpr uint32_t kSampleLimit = 200'000;
  constexpr uint32_t kChannelMask = 0x0F;

  {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    g_expected_samples = kSampleLimit;
    g_received_samples = 0;
    g_chunks.clear();
  }

  bitoscope::host::pipe::CaptureRequest la_cmd;
  la_cmd.sample_rate_milli_hz = kSampleRateHz * 1000;
  la_cmd.channel_mask = kChannelMask;
  la_cmd.sample_limit = 0;
  la_cmd.mode = bitoscope::ipc::la::AcquisitionMode::streaming;
  la_cmd.capture_id = (*reader)->captureId();
  la_cmd.bulk_ring_name = (*reader)->name();

  std::printf("Starting capture: 1 MHz, channels 0-3, streaming, stopping after %u "
              "samples...\n",
              kSampleLimit);
  if (auto started = la.start(la_cmd); !started) {
    std::fprintf(stderr, "FAIL: could not start capture: %s\n", started.error().c_str());
    stopGenerator(siggen, kChannelMask);
    return 1;
  }

  bool got_all_samples;
  {
    std::unique_lock<std::mutex> lock(g_sample_mutex);
    got_all_samples = g_sample_cv.wait_for(lock, kSampleTimeout, [] {
      return g_received_samples >= g_expected_samples;
    });
  }

  (void)la.stop();
  stopGenerator(siggen, kChannelMask);
  (*reader)->stop();
  std::printf("Bulk ring: peak %llu slot(s) in use%s%s\n",
              static_cast<unsigned long long>((*reader)->peakSlotsInUse()),
              (*reader)->endReason().empty() ? "" : "; ", (*reader)->endReason().c_str());
  if ((*reader)->failed()) {
    std::fprintf(stderr, "FAIL: capture has hole - %s\n", (*reader)->endReason().c_str());
    return 1;
  }

  if (!got_all_samples) {
    std::fprintf(stderr, "FAIL: only received %u/%u samples within %llds.\n",
                 g_received_samples, g_expected_samples,
                 static_cast<long long>(kSampleTimeout.count()));
    return 1;
  }
  std::printf("Received %u samples across %zu chunk(s). Decoding...\n\n",
              g_received_samples, g_chunks.size());

  bool all_pass = true;
  for (size_t i = 0; i < kPorts.size(); ++i) {
    const auto& p = kPorts[i];
    auto bits = extractChannelBits(g_chunks, static_cast<uint32_t>(i),
                                    static_cast<uint32_t>(kPorts.size()));
    const double decoded_hz = estimateFrequencyHz(bits, static_cast<double>(kSampleRateHz));
    const double error = std::abs(decoded_hz - p.frequency_hz) / p.frequency_hz;
    const bool pass = error <= kFrequencyTolerance;
    all_pass &= pass;
    std::printf("Channel %u (P%zu, expected %.1f Hz): decoded %.1f Hz, error %.1f%% -> %s\n",
                p.channel, i, p.frequency_hz, decoded_hz, error * 100.0,
                pass ? "PASS" : "FAIL");
  }

  std::printf("\n%s\n", all_pass ? "ALL CHANNELS PASS" : "SOME CHANNELS FAILED");
  return all_pass ? 0 : 1;
}

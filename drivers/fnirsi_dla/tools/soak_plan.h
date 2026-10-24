/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bulk_ring.h"
#include "pwm_check.h"

namespace fnirsi::tools {

inline constexpr uint32_t kRigPorts = 4;

struct PortSignal {
  uint64_t milli_hz = 0;
  uint32_t duty_percent = 0;
  bool operator==(const PortSignal&) const = default;
};

inline constexpr std::array<PortSignal, kRigPorts> kDefaultRig = {{
    {1'300'000, 25},
    {2'100'000, 50},
    {3'700'000, 75},
    {5'300'000, 40},
}};

// WHY: Device PWM period is round(100 MHz / f) ticks of a sampler-locked clock, not rate / f; measured at 500 MHz.
inline constexpr uint64_t kPwmClockHz = 100'000'000;

uint64_t dlaPwmTicks(uint64_t pwm_milli_hz);

std::expected<PwmShape, std::string> dlaPwmShape(uint64_t sample_rate_hz, uint64_t pwm_milli_hz,
                                                   uint32_t duty_percent);

struct PwmChange {
  uint64_t at_ms = 0;
  uint32_t port = 0;
  std::optional<PortSignal> signal;
  bool operator==(const PwmChange&) const = default;
};

struct SoakOptions {
  uint64_t rate_hz = 500'000'000;
  uint32_t channel_mask = 0x000F;
  uint64_t seconds = 60;
  std::optional<uint32_t> usb_transfer_kib;
  uint64_t ring_mib = 256;
  std::array<PortSignal, kRigPorts> ports = kDefaultRig;
  std::optional<std::string> plugin_id;
  std::optional<std::string> plugin_dir;
  std::array<std::optional<std::pair<uint64_t, uint64_t>>, kRigPorts> period;
  uint32_t check_threads = 0;
  uint32_t edge_tolerance = 1;
  uint32_t run_tolerance = 2;
  std::optional<std::string> timeline_file;
  uint32_t off_ports = 0;
  std::vector<PwmChange> changes;
  bool record_all = false;
  bool help = false;

  uint32_t ringSlots() const { return static_cast<uint32_t>(ring_mib / 4); }
  uint32_t channelCount() const;
  uint32_t inputOf(uint32_t stream_channel) const;
  std::optional<uint32_t> streamChannelOf(uint32_t input) const;
  uint32_t changedPorts() const;
  uint32_t checkedChannels() const;
};

std::expected<SoakOptions, std::string> parseSoakArgs(std::span<const std::string_view> args);

std::string soakUsage();

std::expected<std::vector<CheckedChannel>, std::string> rigChannels(const SoakOptions& options);

struct RecordedInput {
  uint32_t input = 0;
  uint32_t stream_channel = 0;
};
std::vector<RecordedInput> recordedInputs(const SoakOptions& options);

PwmCheckConfig soakCheckConfig(const SoakOptions& options, std::vector<CheckedChannel> checked, uint64_t capture_id);

std::string toleranceText(const SoakOptions& options);

enum class SoakVerdict {
  Clean,
  DeviceLoss,
  HostBehind,
  Stall,
  PhaseOnly,
  Error,
};

std::string_view verdictName(SoakVerdict verdict);
int verdictExitCode(SoakVerdict verdict);

enum class RingEnd {
  Open,
  Ended,
  Failed,
  Invalid,
};

struct DeficitStats {
  bool seen = false;
  int64_t min_us = 0;
  int64_t max_us = 0;
  int64_t last_us = 0;

  void update(int64_t deficit_us);
};

struct SoakFacts {
  std::string setup_error;
  uint64_t chunks = 0;
  uint64_t bytes = 0;
  double wall_seconds = 0.0;
  double checker_busy_seconds = 0.0;
  bool stalled = false;
  RingEnd ring_end = RingEnd::Open;
  bool ring_end_before_stop = false;
  std::string ring_error;
  bitoscope::bulk_ring::FailureKind ring_failure_kind =
      bitoscope::bulk_ring::FailureKind::Unspecified;
  uint64_t failed_at_sample = 0;
  uint64_t failed_lost_samples = 0;
  uint64_t peak_slots = 0;
  uint32_t ring_slots = 0;
  DeficitStats deficit;
  std::optional<double> first_chunk_ms;
};

struct SoakClassification {
  SoakVerdict verdict = SoakVerdict::Error;
  std::string why;
};

std::string ringFailureText(uint64_t at_sample, uint64_t lost_samples, std::string_view stored_reason);

SoakClassification classifySoak(const SoakOptions& options, const SoakFacts& facts, const PwmCheckReport& report);

std::string soakSummaryLine(const SoakOptions& options, const SoakFacts& facts, const PwmCheckReport& report,
                            const SoakClassification& verdict);

int64_t deficitUs(uint64_t wall_us, uint64_t first_sample_index, uint64_t rate_hz);

std::string timelineHeader(const SoakOptions& options);

std::string timelineRow(uint64_t wall_us, uint64_t first_sample_index, uint64_t num_samples, int64_t deficit_us,
                        bool any_deviation);

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "soak_plan.h"

#include <bit>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <numeric>

#include "display_text.h"
#include "fnirsi_protocol.h"
#include "usb_transfer_config.h"

namespace fnirsi::tools {

namespace {

constexpr uint64_t kMaxSeconds = 86'400;
constexpr uint64_t kSlotMiB = 4;  // WHY: Must equal bitoscope_cli's ring slot size.
constexpr uint32_t kMinSlots = 2;
constexpr uint64_t kMaxPwmHz = 20'000'000;
constexpr uint32_t kMaxTolerance = 1000;

std::expected<uint64_t, std::string> number(std::string_view option, std::string_view text) {
  uint64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || text.front() < '0' || text.front() > '9' || ec != std::errc{} ||
      ptr != text.data() + text.size()) {
    return std::unexpected(std::string(option) + ": '" + std::string(text) + "' is not whole number");
  }
  return value;
}

std::expected<uint64_t, std::string> numberIn(std::string_view option, std::string_view text, uint64_t lo,
                                              uint64_t hi) {
  auto value = number(option, text);
  if (!value) return value;
  if (*value < lo || *value > hi) {
    return std::unexpected(std::string(option) + " " + std::string(text) + ": it must be " + std::to_string(lo) +
                           " to " + std::to_string(hi));
  }
  return value;
}

std::expected<std::array<uint64_t, kRigPorts>, std::string> fourNumbers(std::string_view option, std::string_view text,
                                                                        uint64_t lo, uint64_t hi) {
  std::array<uint64_t, kRigPorts> out{};
  size_t count = 0;
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const std::string_view item = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (count == kRigPorts) {
      return std::unexpected(std::string(option) + " " + std::string(text) + ": give one value per port P0-P3, four in all");
    }
    auto value = numberIn(option, item, lo, hi);
    if (!value) return std::unexpected(value.error());
    out[count++] = *value;
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  if (count != kRigPorts) {
    return std::unexpected(std::string(option) + " " + std::string(text) + ": give one value per port P0-P3, four in all");
  }
  return out;
}

std::string rateList() {
  std::string out;
  for (const auto& entry : kRateTable) out += (out.empty() ? "" : ", ") + std::to_string(entry.hz);
  return out;
}

std::string quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) out += c == '"' ? '\'' : (c == '\n' || c == '\r' ? ' ' : c);
  return out + "\"";
}

std::string fixed(double value, int decimals) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", decimals, value);
  return text;
}

double samples(uint64_t num, uint64_t den) { return static_cast<double>(num) / static_cast<double>(den); }

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::string named(const SoakOptions& options, const ChannelReport& channel, const Deviation& deviation,
                  const PwmCheckReport& report) {
  std::string text = describe(deviation, channel.shape, report.edge_tolerance, report.run_tolerance);
  const std::string prefix = "channel " + std::to_string(channel.channel);
  if (text.rfind(prefix, 0) == 0) {
    text = "D" + std::to_string(options.inputOf(channel.channel)) + text.substr(prefix.size());
  }
  return text;
}

std::string inputList(uint32_t mask) {
  std::string out;
  for (uint32_t c = 0; c < 16; ++c) {
    if ((mask >> c) & 1u) out += (out.empty() ? "" : ",") + std::to_string(c);
  }
  return out;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (true) {
    const size_t at = text.find(separator, start);
    out.push_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
    if (at == std::string_view::npos) return out;
    start = at + 1;
  }
}

std::optional<uint32_t> portOf(std::string_view text) {
  if (text.size() != 2 || text[0] != 'P' || text[1] < '0' || text[1] >= static_cast<char>('0' + kRigPorts)) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(text[1] - '0');
}

std::expected<uint32_t, std::string> parseInputs(std::string_view value) {
  const auto bad = [&](const std::string& why) {
    return std::unexpected("--inputs " + std::string(value) + ": " + why +
                           " (give inputs 0-15 and ranges a-b, comma-separated, each input once)");
  };
  uint32_t mask = 0;
  for (const std::string_view item : split(value, ',')) {
    const size_t dash = item.find('-');
    const auto lo = numberIn("--inputs", item.substr(0, dash), 0, 15);
    if (!lo) return bad(lo.error());
    auto hi = lo;
    if (dash != std::string_view::npos) {
      hi = numberIn("--inputs", item.substr(dash + 1), 0, 15);
      if (!hi) return bad(hi.error());
      if (*hi < *lo) return bad("range " + std::string(item) + " runs backwards");
    }
    for (uint64_t c = *lo; c <= *hi; ++c) {
      if ((mask >> c) & 1u) return bad("D" + std::to_string(c) + " is given twice");
      mask |= 1u << c;
    }
  }
  return mask;
}

std::expected<PwmChange, std::string> parseChange(std::string_view value) {
  const auto bad = [&](const std::string& why) {
    return std::unexpected("--change " + std::string(value) + ": " + why +
                           " (give MS:Pp:off or MS:Pp:HZ:DUTY, MS after START, p 0-3, HZ 1-20000000, DUTY 1-99)");
  };
  const auto parts = split(value, ':');
  if (parts.size() != 3 && parts.size() != 4) return bad("malformed change");
  PwmChange change;
  const auto at = number("--change", parts[0]);
  if (!at) return bad(at.error());
  change.at_ms = *at;
  const auto port = portOf(parts[1]);
  if (!port) return bad("'" + std::string(parts[1]) + "' is not port P0-P3");
  change.port = *port;
  if (parts.size() == 3) {
    if (parts[2] != "off") return bad("'" + std::string(parts[2]) + "' is not off");
    return change;
  }
  const auto hz = numberIn("--change", parts[2], 1, kMaxPwmHz);
  if (!hz) return bad(hz.error());
  const auto duty = numberIn("--change", parts[3], 1, 99);
  if (!duty) return bad(duty.error());
  change.signal = PortSignal{*hz * 1000, static_cast<uint32_t>(*duty)};
  return change;
}

}

std::expected<SoakOptions, std::string> parseSoakArgs(std::span<const std::string_view> args) {
  SoakOptions o;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--help" || arg == "-h") {
      o.help = true;
      continue;
    }
    if (arg == "--record-all") {
      o.record_all = true;
      continue;
    }
    const bool takes_value = arg == "--rate-hz" || arg == "--channels" || arg == "--seconds" ||
                             arg == "--usb-transfer-kib" || arg == "--ring-mib" || arg == "--pwm-hz" ||
                             arg == "--duty" || arg == "--period" || arg == "--check-threads" ||
                             arg == "--edge-tolerance" || arg == "--run-tolerance" || arg == "--driver" ||
                             arg == "--driver-dir" || arg == "--timeline" || arg == "--inputs" ||
                             arg == "--change" || arg == "--off";
    if (!takes_value) return std::unexpected("unknown option '" + std::string(arg) + "'; see --help");
    if (i + 1 >= args.size()) return std::unexpected(std::string(arg) + " needs value");
    const std::string_view value = args[++i];
    if (arg == "--rate-hz") {
      auto rate = number(arg, value);
      if (!rate) return std::unexpected(rate.error());
      if (!rateIndexForHz(*rate)) {
        return std::unexpected("--rate-hz " + std::string(value) + ": not DLA rate (" + rateList() + ")");
      }
      o.rate_hz = *rate;
    } else if (arg == "--channels") {
      auto n = number(arg, value);
      if (!n) return std::unexpected(n.error());
      if (*n < 1 || *n > 16) {
        return std::unexpected("--channels " + std::string(value) + ": DLA has 1 to 16 channels");
      }
      o.channel_mask = static_cast<uint32_t>((uint64_t{1} << *n) - 1u);
    } else if (arg == "--inputs") {
      auto mask = parseInputs(value);
      if (!mask) return std::unexpected(mask.error());
      o.channel_mask = *mask;
    } else if (arg == "--change") {
      auto change = parseChange(value);
      if (!change) return std::unexpected(change.error());
      o.changes.push_back(*change);
    } else if (arg == "--off") {
      const auto port = portOf(value);
      if (!port) return std::unexpected("--off " + std::string(value) + ": give port P0-P3");
      o.off_ports |= 1u << *port;
    } else if (arg == "--seconds") {
      auto s = numberIn(arg, value, 1, kMaxSeconds);
      if (!s) return std::unexpected(s.error());
      o.seconds = *s;
    } else if (arg == "--usb-transfer-kib") {
      auto kib = numberIn(arg, value, kMinUsbTransferKib, kMaxUsbTransferKib);
      if (!kib) return std::unexpected(kib.error());
      o.usb_transfer_kib = static_cast<uint32_t>(*kib);
    } else if (arg == "--ring-mib") {
      auto mib = number(arg, value);
      if (!mib) return std::unexpected(mib.error());
      if (*mib % kSlotMiB != 0 || *mib / kSlotMiB < kMinSlots || *mib / kSlotMiB > UINT32_MAX) {
        return std::unexpected("--ring-mib " + std::string(value) + ": ring is whole " + std::to_string(kSlotMiB) +
                               " MiB slots, at least " + std::to_string(kMinSlots));
      }
      o.ring_mib = *mib;
    } else if (arg == "--pwm-hz") {
      auto hz = fourNumbers(arg, value, 1, kMaxPwmHz);
      if (!hz) return std::unexpected(hz.error());
      for (uint32_t p = 0; p < kRigPorts; ++p) o.ports[p].milli_hz = (*hz)[p] * 1000;
    } else if (arg == "--duty") {
      auto duty = fourNumbers(arg, value, 1, 99);
      if (!duty) return std::unexpected(duty.error());
      for (uint32_t p = 0; p < kRigPorts; ++p) o.ports[p].duty_percent = static_cast<uint32_t>((*duty)[p]);
    } else if (arg == "--period") {
      const size_t eq = value.find('=');
      const std::string_view input = value.substr(0, eq == std::string_view::npos ? 0 : eq);
      if (eq == std::string_view::npos || input.size() != 2 || input[0] != 'D' || input[1] < '0' ||
          input[1] >= static_cast<char>('0' + kRigPorts)) {
        return std::unexpected("--period " + std::string(value) + ": give Dc=NUM/DEN for input D0-D3 that rig drives");
      }
      auto period = parseSamples(value.substr(eq + 1));
      if (!period) return std::unexpected("--period " + std::string(value) + ": " + period.error());
      if (period->first == 0) return std::unexpected("--period " + std::string(value) + ": period of zero samples");
      o.period[static_cast<size_t>(input[1] - '0')] = *period;
    } else if (arg == "--check-threads") {
      auto t = numberIn(arg, value, 0, kRigPorts);
      if (!t) return std::unexpected(t.error());
      o.check_threads = static_cast<uint32_t>(*t);
    } else if (arg == "--driver") {
      if (value.empty()) return std::unexpected("--driver needs device id, e.g. usb-1-15");
      o.plugin_id = std::string(value);
    } else if (arg == "--driver-dir") {
      if (value.empty()) return std::unexpected("--driver-dir needs directory");
      o.plugin_dir = std::string(value);
    } else if (arg == "--edge-tolerance") {
      auto t = numberIn(arg, value, 0, kMaxTolerance);
      if (!t) return std::unexpected(t.error());
      o.edge_tolerance = static_cast<uint32_t>(*t);
    } else if (arg == "--run-tolerance") {
      auto t = numberIn(arg, value, 0, kMaxTolerance);
      if (!t) return std::unexpected(t.error());
      o.run_tolerance = static_cast<uint32_t>(*t);
    } else {
      o.timeline_file = std::string(value);
    }
  }
  std::stable_sort(o.changes.begin(), o.changes.end(),
                   [](const PwmChange& a, const PwmChange& b) { return a.at_ms < b.at_ms; });
  for (const PwmChange& change : o.changes) {
    if (change.at_ms >= o.seconds * 1000) {
      return std::unexpected("--change at " + std::to_string(change.at_ms) + " ms: run lasts " +
                             std::to_string(o.seconds) + " s (--seconds)");
    }
  }
  if ((o.channel_mask & ((1u << kRigPorts) - 1u)) == 0) {
    return std::unexpected("--inputs " + inputList(o.channel_mask) +
                           ": capture holds none of rig's inputs D0-D3, so nothing can be checked");
  }
  if (o.checkedChannels() == 0) {
    return std::unexpected("--change/--off: every rig input in capture (D" + inputList(o.channel_mask & 0xFu) +
                           ") has its port changed; leave one unchanged, whose check proves stream whole");
  }
  for (uint32_t c = 0; c < kRigPorts; ++c) {
    if (!o.period[c]) continue;
    if (!o.streamChannelOf(c)) {
      return std::unexpected("--period D" + std::to_string(c) + ": capture (inputs " + inputList(o.channel_mask) +
                             ") does not hold D" + std::to_string(c));
    }
    if ((o.changedPorts() >> c) & 1u) {
      return std::unexpected("--period D" + std::to_string(c) + ": its port P" + std::to_string(c) +
                             " is changed during run (--change/--off), so D" + std::to_string(c) +
                             " is recorded, not checked");
    }
  }
  if (o.check_threads > o.checkedChannels()) {
    return std::unexpected("--check-threads " + std::to_string(o.check_threads) + ": at most one per checked input, " +
                           std::to_string(o.checkedChannels()) + " here");
  }
  return o;
}

std::string soakUsage() {
  return "usage: fnirsi_dla_max_throughput_soak_test [options]\n"
         "Starts fnirsi_dla driver on DLA, streams from it through bulk ring and checks every\n"
         "edge and run of PWM looped back from P0-P3 into D0-D3.\n"
         "  --driver ID           DLA to use, by device id fnirsi_dla lists (e.g. usb-1-15;\n"
         "                        default: sole one connected - several and none named is refused)\n"
         "  --driver-dir DIR      where fnirsi_dla is (default: this build's driver directory, drivers/\n"
         "                        next to bitoscope_cli)\n"
         "  --rate-hz N           sample rate, DLA rate (default 500000000)\n"
         "  --channels N          D0..D(N-1), N = 1 to 16 (default 4; DLA pads count it cannot\n"
         "                        stream to 2, 4, 8 or 16 and drops pad channels); D0-D3 are checked\n"
         "  --inputs LIST         inputs to capture instead, e.g. 0,2,3,9 or 0-2,5 (any of D0-D15, at\n"
         "                        least one of D0-D3); stream holds them in input order\n"
         "  --seconds N           how long to stream (default 60)\n"
         "  --usb-transfer-kib N  driver's USB transfer size, set as its usb_transfer_kib property\n"
         "                        (default: driver's own, 8, each about 0.1 s of data, 16 KiB to 1 MiB)\n"
         "  --ring-mib N          bulk ring, whole 4 MiB slots (default 256)\n"
         "  --pwm-hz F0,F1,F2,F3  PWM on P0-P3, whole Hz (default 1300,2100,3700,5300)\n"
         "  --duty D0,D1,D2,D3    their duty, percent (default 25,50,75,40: acceptance runs' set)\n"
         "  --period Dc=NUM/DEN   check Dc against exact period of NUM/DEN samples instead of device's\n"
         "                        PWM clock model (round(100 MHz / f) ticks of 10 ns)\n"
         "  --check-threads N     threads checking channels, 0 = on ring reader's (default 0)\n"
         "  --edge-tolerance N    samples each edge may lie off its prediction (default 1)\n"
         "  --run-tolerance N     samples each run may differ from first run of its level on its input\n"
         "                        (default 2: each run has two edges, and each may sit up to --edge-tolerance\n"
         "                        off its own phase)\n"
         "  --off Pp              do not start port Pp at beginning (--change may switch it on)\n"
         "  --change MS:Pp:HZ:DUTY  MS milliseconds after START, restart port Pp at HZ and DUTY percent (on,\n"
         "                        if it was off), by siggen facet's commands; --change MS:Pp:off stops it.\n"
         "                        Any number of them. Input whose port --off or --change touches is not\n"
         "                        checked: its runs are printed in segments (tools/run_segments.h)\n"
         "  --record-all          also print every unchanged rig input's runs in segments (they stay checked)\n"
         "  --timeline FILE       write CSV of every chunk read from ring: wall_us, first_sample_index,\n"
         "                        num_samples, deficit_us (wall_us behind chunk's own sample time) and\n"
         "                        checker's running any_deviation (default: no file written)\n"
         "Exit code: 0 CLEAN, 2 DEVICE_LOSS, 3 HOST_BEHIND, 4 STALL, 5 PHASE_ONLY, 1 ERROR.\n";
}

std::expected<std::vector<CheckedChannel>, std::string> rigChannels(const SoakOptions& options) {
  std::vector<CheckedChannel> out;
  for (uint32_t c = 0; c < kRigPorts; ++c) {
    const auto stream_channel = options.streamChannelOf(c);
    if (!stream_channel || ((options.changedPorts() >> c) & 1u)) continue;
    const PortSignal& port = options.ports[c];
    const auto shape = options.period[c]
                           ? makePwmShape(options.period[c]->first, options.period[c]->second, port.duty_percent)
                           : dlaPwmShape(options.rate_hz, port.milli_hz, port.duty_percent);
    if (!shape) return std::unexpected("D" + std::to_string(c) + " (P" + std::to_string(c) + "): " + shape.error());
    out.push_back(CheckedChannel{*stream_channel, *shape});
  }
  return out;
}

uint32_t SoakOptions::channelCount() const { return static_cast<uint32_t>(std::popcount(channel_mask)); }
uint32_t SoakOptions::inputOf(uint32_t stream_channel) const {
  uint32_t k = 0;
  for (uint32_t c = 0; c < 16; ++c) {
    if (((channel_mask >> c) & 1u) && k++ == stream_channel) return c;
  }
  return 16;
}

std::optional<uint32_t> SoakOptions::streamChannelOf(uint32_t input) const {
  if (input >= 16 || ((channel_mask >> input) & 1u) == 0) return std::nullopt;
  return static_cast<uint32_t>(std::popcount(channel_mask & ((1u << input) - 1u)));
}

uint32_t SoakOptions::changedPorts() const {
  uint32_t changed = off_ports;
  for (const PwmChange& change : changes) changed |= 1u << change.port;
  return changed;
}

uint32_t SoakOptions::checkedChannels() const {
  return static_cast<uint32_t>(std::popcount(channel_mask & ((1u << kRigPorts) - 1u) & ~changedPorts()));
}

std::vector<RecordedInput> recordedInputs(const SoakOptions& options) {
  std::vector<RecordedInput> out;
  for (uint32_t c = 0; c < kRigPorts; ++c) {
    const auto stream_channel = options.streamChannelOf(c);
    if (stream_channel && (options.record_all || ((options.changedPorts() >> c) & 1u))) {
      out.push_back(RecordedInput{c, *stream_channel});
    }
  }
  return out;
}

uint64_t dlaPwmTicks(uint64_t pwm_milli_hz) {
  if (pwm_milli_hz == 0) return 0;
  constexpr uint64_t kClockMilli = kPwmClockHz * 1000;
  if (pwm_milli_hz > kClockMilli * 2) return 0;
  return (2 * kClockMilli + pwm_milli_hz) / (2 * pwm_milli_hz);
}

std::expected<PwmShape, std::string> dlaPwmShape(uint64_t sample_rate_hz, uint64_t pwm_milli_hz,
                                                   uint32_t duty_percent) {
  if (sample_rate_hz == 0) return std::unexpected(std::string("sample rate of 0 Hz"));
  if (pwm_milli_hz == 0) return std::unexpected(std::string("PWM frequency of 0 Hz"));
  const uint64_t ticks = dlaPwmTicks(pwm_milli_hz);
  if (ticks == 0) {
    return std::unexpected("PWM of " + std::to_string(pwm_milli_hz) + " mHz is under one tick of DLA's " +
                           std::to_string(kPwmClockHz) + " Hz PWM clock");
  }
  // WHY: Reduce by gcd first so the product stays small.
  const uint64_t g = std::gcd(sample_rate_hz, kPwmClockHz);
  const uint64_t per_tick_num = sample_rate_hz / g;
  const uint64_t per_tick_den = kPwmClockHz / g;
  if (ticks > UINT64_MAX / per_tick_num) {
    return std::unexpected("PWM period of " + std::to_string(ticks) + " ticks at " + std::to_string(sample_rate_hz) +
                           " Hz is too large to check exactly");
  }
  return makePwmShape(ticks * per_tick_num, per_tick_den, duty_percent);
}

PwmCheckConfig soakCheckConfig(const SoakOptions& options, std::vector<CheckedChannel> checked, uint64_t capture_id) {
  PwmCheckConfig config;
  config.num_channels = options.channelCount();
  config.sample_rate_milli_hz = options.rate_hz * 1000;
  config.capture_id = capture_id;
  config.channels = std::move(checked);
  config.edge_tolerance = options.edge_tolerance;
  config.run_tolerance = options.run_tolerance;
  config.run_reference = RunReference::FirstRun;
  config.workers = options.check_threads;
  return config;
}

std::string toleranceText(const SoakOptions& options) {
  return "edge tolerance " + std::to_string(options.edge_tolerance) + ", run tolerance " +
         std::to_string(options.run_tolerance) + " sample(s) of first run of its level";
}

std::string_view verdictName(SoakVerdict verdict) {
  switch (verdict) {
    case SoakVerdict::Clean: return "CLEAN";
    case SoakVerdict::DeviceLoss: return "DEVICE_LOSS";
    case SoakVerdict::HostBehind: return "HOST_BEHIND";
    case SoakVerdict::Stall: return "STALL";
    case SoakVerdict::PhaseOnly: return "PHASE_ONLY";
    case SoakVerdict::Error: return "ERROR";
  }
  return "ERROR";
}

int verdictExitCode(SoakVerdict verdict) {
  switch (verdict) {
    case SoakVerdict::Clean: return 0;
    case SoakVerdict::DeviceLoss: return 2;
    case SoakVerdict::HostBehind: return 3;
    case SoakVerdict::Stall: return 4;
    case SoakVerdict::PhaseOnly: return 5;
    case SoakVerdict::Error: return 1;
  }
  return 1;
}

std::string ringFailureText(uint64_t at_sample, uint64_t lost_samples, std::string_view stored_reason) {
  return "driver could not deliver from sample " + std::to_string(at_sample) + " on (" + std::to_string(lost_samples) +
         " samples lost): " + bitoscope::display_text::displayText(stored_reason);
}

SoakClassification classifySoak(const SoakOptions& options, const SoakFacts& f, const PwmCheckReport& report) {
  using V = SoakVerdict;
  if (!f.setup_error.empty()) return {V::Error, "run did not get going: " + f.setup_error};
  // WHY: Wrong data ranks first: whatever else happened, what arrived was not the signal.
  if (report.stream_error) return {V::DeviceLoss, "stream broke: " + report.stream_error->message};
  for (const auto& ch : report.channels) {
    if (ch.run) return {V::DeviceLoss, "wrong data: " + named(options, ch, *ch.run, report)};
    if (ch.no_edge) return {V::DeviceLoss, "wrong data: " + named(options, ch, *ch.no_edge, report)};
  }
  if (f.ring_end == RingEnd::Failed) {
    const std::string record = ringFailureText(f.failed_at_sample, f.failed_lost_samples, f.ring_error);
    // WHY: Host-side failures are told by FailureKind, never by words; the 'no USB transfer' stall wording
    // below is matched as the driver writes it.
    using bitoscope::bulk_ring::FailureKind;
    if (f.ring_failure_kind == FailureKind::RingFull || f.ring_failure_kind == FailureKind::ConsumerStale) {
      return {V::HostBehind, record + "; ring held up to " + std::to_string(f.peak_slots) + " of " +
                                 std::to_string(f.ring_slots) + " slots"};
    }
    if (contains(f.ring_error, "no USB transfer")) return {V::Stall, "device stopped sending: " + record};
    return {V::DeviceLoss, record};
  }
  if (f.ring_end == RingEnd::Invalid) return {V::Error, "bulk ring broke its protocol: " + f.ring_error};
  if (f.chunks == 0) return {V::Stall, "no chunk arrived"};
  if (f.stalled) return {V::Stall, "chunks stopped arriving after " + std::to_string(f.chunks) + " of them"};
  if (f.ring_end == RingEnd::Ended && f.ring_end_before_stop) {
    return {V::Stall, "driver ended stream before tool stopped it"};
  }
  for (const auto& ch : report.channels) {
    if (ch.phase) {
      return {V::PhaseOnly, "every run stayed in its window, but " + named(options, ch, *ch.phase, report) +
                                "; compare measured periods with expected ones before calling it data loss"};
    }
  }
  return {V::Clean, "every chunk arrived in order, and every checked edge and run was where PWM put it"};
}

std::string soakSummaryLine(const SoakOptions& options, const SoakFacts& f, const PwmCheckReport& report,
                            const SoakClassification& verdict) {
  const auto ringEnd = [](RingEnd end) {
    switch (end) {
      case RingEnd::Open: return "open";
      case RingEnd::Ended: return "ended";
      case RingEnd::Failed: return "failed";
      case RingEnd::Invalid: return "invalid";
    }
    return "open";
  };
  const bool failed = f.ring_end == RingEnd::Failed;
  std::string line = "SOAK_RESULT verdict=" + std::string(verdictName(verdict.verdict));
  line += " rate_hz=" + std::to_string(options.rate_hz);
  line += " channels=" + std::to_string(options.channelCount());
  line += " inputs=" + inputList(options.channel_mask);
  line += " bits_per_s=" + std::to_string(options.rate_hz * options.channelCount());
  line += " seconds=" + std::to_string(options.seconds);
  line += " usb_transfer_kib=" + (options.usb_transfer_kib ? std::to_string(*options.usb_transfer_kib) : "default");
  line += " ring_slots=" + std::to_string(f.ring_slots);
  line += " bytes=" + std::to_string(f.bytes);
  line += " chunks=" + std::to_string(f.chunks);
  line += " wall_s=" + fixed(f.wall_seconds, 3);
  line += " achieved_mb_s=" + fixed(f.wall_seconds > 0 ? static_cast<double>(f.bytes) / 1e6 / f.wall_seconds : 0.0, 1);
  line += " signal_s=" + fixed(static_cast<double>(report.samples_per_channel) / static_cast<double>(options.rate_hz), 3);
  line += " peak_slots=" + std::to_string(f.peak_slots);
  line += " ring_end=" + std::string(ringEnd(f.ring_end));
  line += " failed_at=" + (failed ? std::to_string(f.failed_at_sample) : std::string("-"));
  line += " failed_lost=" + (failed ? std::to_string(f.failed_lost_samples) : std::string("-"));
  line += " ring_error=" + quoted(f.ring_error);
  line += " stream_error=" + quoted(report.stream_error ? report.stream_error->message : "");
  line += " check_threads=" + std::to_string(options.check_threads);
  line += " checker_busy_pct=" + fixed(f.wall_seconds > 0 ? 100.0 * f.checker_busy_seconds / f.wall_seconds : 0.0, 1);
  line += " edge_tolerance=" + std::to_string(options.edge_tolerance);
  line += " run_tolerance=" + std::to_string(options.run_tolerance);
  for (const auto& ch : report.channels) {
    const std::string d = " D" + std::to_string(options.inputOf(ch.channel));
    std::string result = "OK";
    if (const auto first = ch.firstDeviation()) {
      const char* kind = first->kind == DeviationKind::EdgeOffPhase ? "PHASE"
                         : first->kind == DeviationKind::RunLength  ? "RUN"
                                                                    : "NOEDGE";
      result = std::string(kind) + "@" + std::to_string(first->sample);
    }
    line += d + "=" + result;
    line += d + "_edges=" + std::to_string(ch.edges);
    line += d + "_period=" + fixed(ch.measuredPeriod(), 3);
    line += d + "_expected_period=" + fixed(samples(ch.shape.period_num, ch.shape.den), 3);
    line += d + "_high=" + fixed(ch.high_runs.mean(), 3);
    line += d + "_expected_high=" + fixed(samples(ch.shape.high_num, ch.shape.den), 3);
    line += d + "_low=" + fixed(ch.low_runs.mean(), 3);
    line += d + "_expected_low=" + fixed(samples(ch.shape.lowNum(), ch.shape.den), 3);
    line += d + "_high_min=" + std::to_string(ch.high_runs.min) + d + "_high_max=" + std::to_string(ch.high_runs.max);
    line += d + "_low_min=" + std::to_string(ch.low_runs.min) + d + "_low_max=" + std::to_string(ch.low_runs.max);
    line += d + "_edges_exact=" + std::to_string(ch.edges_exact) + d + "_edges_off1=" + std::to_string(ch.edges_off_by_one);
    line += d + "_max_phase_error=" + std::to_string(ch.max_phase_error);
    line += d + "_last_phase_error=" + std::to_string(ch.last_phase_error);
  }
  line += " why=" + quoted(verdict.why);
  return line;
}

void DeficitStats::update(int64_t deficit_us) {
  if (!seen) {
    seen = true;
    min_us = deficit_us;
    max_us = deficit_us;
  } else {
    if (deficit_us < min_us) min_us = deficit_us;
    if (deficit_us > max_us) max_us = deficit_us;
  }
  last_us = deficit_us;
}

int64_t deficitUs(uint64_t wall_us, uint64_t first_sample_index, uint64_t rate_hz) {
  // WHY: Split into whole seconds and remainder so first_sample_index * 1'000'000 is never formed (overflow).
  const uint64_t whole_seconds = first_sample_index / rate_hz;
  const uint64_t remainder = first_sample_index % rate_hz;
  const uint64_t sample_time_us = whole_seconds * 1'000'000ull + (remainder * 1'000'000ull) / rate_hz;
  return static_cast<int64_t>(wall_us) - static_cast<int64_t>(sample_time_us);
}

std::string timelineHeader(const SoakOptions& options) {
  return "# fnirsi_dla soak --timeline: rate_hz=" + std::to_string(options.rate_hz) +
         " channels=" + std::to_string(options.channelCount()) + " inputs=" + inputList(options.channel_mask) +
         "\n" +
         "wall_us,first_sample_index,num_samples,deficit_us,any_deviation\n";
}

std::string timelineRow(uint64_t wall_us, uint64_t first_sample_index, uint64_t num_samples, int64_t deficit_us,
                        bool any_deviation) {
  return std::to_string(wall_us) + "," + std::to_string(first_sample_index) + "," + std::to_string(num_samples) +
         "," + std::to_string(deficit_us) + "," + (any_deviation ? "1" : "0") + "\n";
}

}

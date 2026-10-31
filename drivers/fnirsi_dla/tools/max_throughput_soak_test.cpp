/*
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "console_code_page.h"
#include "display_text.h"
#include "dla_session.h"
#include "fnirsi_protocol.h"
#include "pwm_check.h"
#include "ring_reader.h"
#include "run_segments.h"
#include "sample_encoding.h"
#include "soak_plan.h"

static_assert(fnirsi::tools::kChannelMajorPackedEncoding ==
                  static_cast<uint32_t>(bitoscope::host::SampleEncoding::ChannelMajorPacked),
              "checker reads ChannelMajorPacked by value ring's chunk records carry");

namespace {

namespace tools = fnirsi::tools;
using Clock = std::chrono::steady_clock;

constexpr auto kCommandTimeout = std::chrono::seconds(15);
constexpr auto kDriverStartTimeout = std::chrono::seconds(30);
constexpr auto kStallThreshold = std::chrono::seconds(3);
// WHY: First-chunk timeout must exceed one transfer's fill: seconds at 1 MHz for a large --usb-transfer-kib.
constexpr auto kFirstChunkTimeout = std::chrono::seconds(10);
constexpr auto kEndWait = std::chrono::seconds(3);
constexpr auto kPoll = std::chrono::milliseconds(100);
constexpr auto kProgressEvery = std::chrono::seconds(5);
constexpr uint64_t kSlotBytes = 4 * 1024 * 1024;

std::optional<std::string> startGenerator(bitoscope::host::pipe::SiggenClient& siggen, uint32_t port,
                                          const tools::PortSignal& signal) {
  const uint32_t mask = 1u << port;
  if (auto configured = siggen.configureSquare(mask, {signal.milli_hz, signal.duty_percent}); !configured) {
    return "P" + std::to_string(port) + ": " + configured.error();
  }
  if (auto started = siggen.start(mask, bitoscope::host::pipe::StartMode::Independent); !started) {
    return "P" + std::to_string(port) + ": " + started.error();
  }
  return std::nullopt;
}

void stopGenerator(bitoscope::host::pipe::SiggenClient& siggen, uint32_t channel_mask) {
  (void)siggen.stop(channel_mask);
}

struct Progress {
  std::atomic<uint64_t> chunks{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<int64_t> last_chunk_ns{0};
  std::atomic<int64_t> busy_ns{0};
};

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

double secondsBetween(int64_t from_ns, int64_t to_ns) { return static_cast<double>(to_ns - from_ns) / 1e9; }

tools::RingEnd ringEndOf(tools::RingReader::End end) {
  switch (end) {
    case tools::RingReader::End::Open: return tools::RingEnd::Open;
    case tools::RingReader::End::Ended: return tools::RingEnd::Ended;
    case tools::RingReader::End::Failed: return tools::RingEnd::Failed;
    case tools::RingReader::End::Invalid: return tools::RingEnd::Invalid;
  }
  return tools::RingEnd::Open;
}

double samples(uint64_t num, uint64_t den) { return static_cast<double>(num) / static_cast<double>(den); }

std::string inputText(uint32_t mask) {
  std::string out;
  for (uint32_t c = 0; c < 16; ++c) {
    if ((mask >> c) & 1u) out += (out.empty() ? "D" : ",D") + std::to_string(c);
  }
  return out;
}

std::string signalText(const std::optional<tools::PortSignal>& signal) {
  if (!signal) return "off";
  char text[64];
  std::snprintf(text, sizeof(text), "%.3f Hz, %u%% duty", static_cast<double>(signal->milli_hz) / 1000.0,
                signal->duty_percent);
  return text;
}

void printPlan(const tools::SoakOptions& o, const std::vector<tools::CheckedChannel>& checked) {
  const uint64_t bits = o.rate_hz * o.channelCount();
  std::printf("Plan: %llu Hz x %u channels (%s) = %.3f Gbit/s (%.1f MB/s of payload), streaming, %llu s\n",
              static_cast<unsigned long long>(o.rate_hz), o.channelCount(), inputText(o.channel_mask).c_str(),
              static_cast<double>(bits) / 1e9, static_cast<double>(bits) / 8e6,
              static_cast<unsigned long long>(o.seconds));
  if (o.usb_transfer_kib) {
    std::printf("  USB transfers: %u KiB each, set as analyzer's usb_transfer_kib property (it stays set in "
                "driver until it restarts)\n",
                *o.usb_transfer_kib);
  } else {
    std::printf("  USB transfers: automatic (8, each about 0.1 s of data, at most 1 MiB)\n");
  }
  std::printf("  Ring: %u slots of 4 MiB; checker: %s; %s\n", o.ringSlots(),
              o.check_threads == 0 ? "on ring reader's thread" : (std::to_string(o.check_threads) + " thread(s)").c_str(),
              tools::toleranceText(o).c_str());
  for (uint32_t p = 0; p < tools::kRigPorts; ++p) {
    const bool off = ((o.off_ports >> p) & 1u) != 0;
    const bool changed = ((o.changedPorts() >> p) & 1u) != 0;
    std::printf("  P%u: %s%s%s\n", p, off ? "off at start" : signalText(o.ports[p]).c_str(),
                o.streamChannelOf(p) ? "" : " (its input is not in this capture)",
                changed ? " (changed during run: its input is recorded, not checked)" : "");
  }
  for (const auto& change : o.changes) {
    std::printf("  at %llu ms: P%u -> %s\n", static_cast<unsigned long long>(change.at_ms), change.port,
                signalText(change.signal).c_str());
  }
  for (const auto& c : checked) {
    const uint32_t input = o.inputOf(c.channel);
    std::printf("  D%u (stream channel %u) checked: period %s, high %s, low %s samples%s\n", input, c.channel,
                tools::formatSamples(c.shape.period_num, c.shape.den).c_str(),
                tools::formatSamples(c.shape.high_num, c.shape.den).c_str(),
                tools::formatSamples(c.shape.lowNum(), c.shape.den).c_str(),
                o.period[input] ? " (from --period)" : " (device's PWM clock model (100 MHz ticks))");
  }
}

void printSummary(const tools::SoakOptions& o, const tools::SoakFacts& f, const tools::PwmCheckReport& report,
                  const tools::SoakClassification& verdict) {
  const uint64_t bits = o.rate_hz * o.channelCount();
  std::printf("\n=== Summary ===\n");
  std::printf("Requested: %llu Hz x %u channels (%s) = %.3f Gbit/s (%.1f MB/s) for %llu s; USB transfers: %s; ring "
              "%u x 4 MiB\n",
              static_cast<unsigned long long>(o.rate_hz), o.channelCount(), inputText(o.channel_mask).c_str(),
              static_cast<double>(bits) / 1e9,
              static_cast<double>(bits) / 8e6, static_cast<unsigned long long>(o.seconds),
              o.usb_transfer_kib ? (std::to_string(*o.usb_transfer_kib) + " KiB").c_str() : "automatic",
              f.ring_slots);
  const double achieved = f.wall_seconds > 0 ? static_cast<double>(f.bytes) / 1e6 / f.wall_seconds : 0.0;
  std::printf("Received: %.1f MB in %llu chunks over %.3f s = %.1f MB/s; that is %.3f s of signal (samples per "
              "channel / rate)\n",
              static_cast<double>(f.bytes) / 1e6, static_cast<unsigned long long>(f.chunks), f.wall_seconds, achieved,
              static_cast<double>(report.samples_per_channel) / static_cast<double>(o.rate_hz));
  if (f.first_chunk_ms) std::printf("First chunk: %.1f ms after START was sent\n", *f.first_chunk_ms);
  std::printf("Checker: busy %.1f%% of time%s\n",
              f.wall_seconds > 0 ? 100.0 * f.checker_busy_seconds / f.wall_seconds : 0.0,
              o.check_threads == 0 ? " (on ring reader's thread)" : "");
  if (f.deficit.seen) {
    std::printf("Deficit (wall clock behind chunk's own sample time): min %.3f ms, max %.3f ms, last %.3f ms%s\n",
                static_cast<double>(f.deficit.min_us) / 1000.0, static_cast<double>(f.deficit.max_us) / 1000.0,
                static_cast<double>(f.deficit.last_us) / 1000.0, o.timeline_file ? "" : " (--timeline not written)");
  }
  std::printf("Ring: peak %llu of %u slots in use; ", static_cast<unsigned long long>(f.peak_slots), f.ring_slots);
  switch (f.ring_end) {
    case tools::RingEnd::Open: std::printf("still open when tool stopped reading\n"); break;
    case tools::RingEnd::Ended:
      std::printf("driver ended it %s\n", f.ring_end_before_stop ? "BEFORE tool stopped it" : "after STOP");
      break;
    case tools::RingEnd::Failed:
      std::printf("driver recorded FAILED %s: from sample %llu on (%llu samples lost): %s\n",
                  f.ring_end_before_stop ? "during run" : "after STOP",
                  static_cast<unsigned long long>(f.failed_at_sample),
                  static_cast<unsigned long long>(f.failed_lost_samples),
                  bitoscope::display_text::displayText(f.ring_error).c_str());
      break;
    case tools::RingEnd::Invalid:
      std::printf("BROKE its protocol: %s\n", bitoscope::display_text::displayText(f.ring_error).c_str());
      break;
  }
  if (f.stalled) std::printf("STALL: chunks stopped arriving (after %llu)\n", static_cast<unsigned long long>(f.chunks));
  if (report.stream_error) std::printf("Stream: %s\n", report.stream_error->message.c_str());
  std::printf("Checked inputs (%s):\n", tools::toleranceText(o).c_str());
  for (const auto& ch : report.channels) {
    const uint32_t input = o.inputOf(ch.channel);
    const auto& port = o.ports[input];
    std::printf("  D%u (P%u, %.3f Hz, %u%%): %s; %llu edges, %llu phase-checked (%llu exact, %llu one sample off; "
                "largest offset %llu, at last edge %lld)\n",
                input, input, static_cast<double>(port.milli_hz) / 1000.0, port.duty_percent,
                ch.firstDeviation() ? "DEVIATION" : "OK", static_cast<unsigned long long>(ch.edges),
                static_cast<unsigned long long>(ch.edges_phase_checked), static_cast<unsigned long long>(ch.edges_exact),
                static_cast<unsigned long long>(ch.edges_off_by_one),
                static_cast<unsigned long long>(ch.max_phase_error), static_cast<long long>(ch.last_phase_error));
    for (const auto* d : {&ch.phase, &ch.run, &ch.no_edge}) {
      if (d->has_value()) {
        std::printf("      %s\n", tools::describe(**d, ch.shape, o.edge_tolerance, o.run_tolerance).c_str());
      }
    }
    std::printf("      period: measured %.4f, expected %.4f samples\n", ch.measuredPeriod(),
                samples(ch.shape.period_num, ch.shape.den));
    std::printf("      high runs: mean %.4f (min %llu, max %llu, %llu runs), expected %.4f\n", ch.high_runs.mean(),
                static_cast<unsigned long long>(ch.high_runs.min), static_cast<unsigned long long>(ch.high_runs.max),
                static_cast<unsigned long long>(ch.high_runs.count), samples(ch.shape.high_num, ch.shape.den));
    std::printf("      low runs:  mean %.4f (min %llu, max %llu, %llu runs), expected %.4f\n", ch.low_runs.mean(),
                static_cast<unsigned long long>(ch.low_runs.min), static_cast<unsigned long long>(ch.low_runs.max),
                static_cast<unsigned long long>(ch.low_runs.count), samples(ch.shape.lowNum(), ch.shape.den));
  }
  std::printf("Verdict: %s - %s\n", std::string(tools::verdictName(verdict.verdict)).c_str(), verdict.why.c_str());
  std::printf("%s\n", tools::soakSummaryLine(o, f, report, verdict).c_str());
  std::fflush(stdout);
}

int run(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  const auto parsed = tools::parseSoakArgs(args);
  if (!parsed) {
    std::fprintf(stderr, "FAIL: %s\n\n%s", parsed.error().c_str(), tools::soakUsage().c_str());
    return tools::verdictExitCode(tools::SoakVerdict::Error);
  }
  const tools::SoakOptions options = *parsed;
  if (options.help) {
    std::printf("%s", tools::soakUsage().c_str());
    return 0;
  }
  const auto checked = tools::rigChannels(options);
  if (!checked) {
    std::fprintf(stderr, "FAIL: %s\n", checked.error().c_str());
    return tools::verdictExitCode(tools::SoakVerdict::Error);
  }
  printPlan(options, *checked);

  tools::SoakFacts facts;
  facts.ring_slots = options.ringSlots();
  tools::PwmCheckReport report;

  // WHY: Open timeline file before touching device or driver so a bad path fails fast.
  std::ofstream timeline_stream;
  if (options.timeline_file) {
    timeline_stream.open(*options.timeline_file, std::ios::out | std::ios::trunc);
    if (!timeline_stream.is_open()) {
      std::fprintf(stderr, "FAIL: --timeline '%s': cannot open file for writing\n",
                  options.timeline_file->c_str());
      return tools::verdictExitCode(tools::SoakVerdict::Error);
    }
    timeline_stream << tools::timelineHeader(options);
  }
  const auto finishWith = [&](const std::string& setup_error) {
    facts.setup_error = setup_error;
    const auto verdict = tools::classifySoak(options, facts, report);
    printSummary(options, facts, report, verdict);
    return tools::verdictExitCode(verdict.verdict);
  };

  tools::DlaSessionOptions session_options;
  session_options.driver_dir = options.driver_dir ? std::filesystem::path(*options.driver_dir)
                                                  : std::filesystem::path(BITOSCOPE_DEFAULT_DRIVER_DIR);
  session_options.device_id = options.driver_device_id.value_or("");
  session_options.start_timeout = kDriverStartTimeout;
  session_options.request_timeout = kCommandTimeout;
  std::printf("Starting %s from %s on %s...\n", std::string(tools::kDlaDriver).c_str(),
              session_options.driver_dir.string().c_str(),
              options.driver_device_id ? ("DLA '" + *options.driver_device_id + "'").c_str() : "connected DLA");
  auto session = tools::DlaSession::open(session_options);
  if (!session) return finishWith(session.error());
  auto& la = (*session)->la();
  auto& siggen = (*session)->siggen();
  const std::string& driver_device_id = (*session)->driverDeviceId();
  if (!la.capabilities().bulk_ring) {
    return finishWith("driver '" + driver_device_id +
                      "' does not write bulk ring, only way samples travel");
  }
  std::printf("Opened '%s' (%s); its la.acquisition writes bulk ring.\n", driver_device_id.c_str(),
              (*session)->deviceName().c_str());

  if (options.usb_transfer_kib) {
    const bitoscope::ipc::props::PropertyValue kib{"usb_transfer_kib", int64_t{*options.usb_transfer_kib}};
    if (auto set = (*session)->setProperties(bitoscope::host::pipe::kLaAcquisitionFacetName, std::span(&kib, 1));
        !set) {
      return finishWith("driver refused usb_transfer_kib=" + std::to_string(*options.usb_transfer_kib) + ": " +
                        set.error());
    }
  }

  constexpr uint32_t kAllPorts = (1u << tools::kRigPorts) - 1;
  std::printf("Starting PWM on P0-P3%s...\n", options.off_ports != 0 ? " (but those --off leaves off)" : "");
  for (uint32_t p = 0; p < tools::kRigPorts; ++p) {
    if ((options.off_ports >> p) & 1u) continue;
    if (auto error = startGenerator(siggen, p, options.ports[p])) {
      stopGenerator(siggen, kAllPorts);
      return finishWith("could not start generator: " + *error);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // WHY: Checker is published to the reader thread before START; no slot can arrive earlier.
  Progress progress;
  std::atomic<tools::PwmStreamChecker*> checker_for_reader{nullptr};
  // WHY: Only the ring reader thread touches deficit stats, so no lock; main reads them after stop() joins it.
  tools::DeficitStats& deficit = facts.deficit;
  bool first_chunk_seen = false;
  int64_t first_chunk_ns = 0;
  // WHY: Recorded segments are written on the reader thread; main reads them only after stop() joins it.
  const auto recorded = tools::recordedInputs(options);
  std::vector<tools::RunSegmentRecorder> recorders;
  for (const auto& r : recorded) {
    auto recorder = tools::RunSegmentRecorder::create(options.channelCount(), r.stream_channel, options.run_tolerance);
    if (!recorder) {
      stopGenerator(siggen, kAllPorts);
      return finishWith("cannot record D" + std::to_string(r.input) + ": " + recorder.error());
    }
    recorders.push_back(std::move(*recorder));
  }
  std::atomic<uint64_t> samples_seen{0};
  auto reader = tools::RingReader::start(
      options.ringSlots(), kSlotBytes,
      [&progress, &checker_for_reader, &deficit, &timeline_stream, &options, &first_chunk_seen, &first_chunk_ns,
       &recorders, &samples_seen](const bitoscope::bulk_ring::ChunkHeader& header, std::span<const uint8_t> payload) {
        const int64_t t0 = nowNs();
        auto* checker = checker_for_reader.load(std::memory_order_acquire);
        if (checker) checker->checkChunk(header, payload);
        for (auto& recorder : recorders) recorder.feed(payload);
        samples_seen.store(header.first_sample_index + header.num_samples, std::memory_order_relaxed);
        const int64_t t1 = nowNs();
        progress.busy_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
        progress.bytes.fetch_add(payload.size(), std::memory_order_relaxed);
        progress.last_chunk_ns.store(t1, std::memory_order_relaxed);
        progress.chunks.fetch_add(1, std::memory_order_release);

        if (!first_chunk_seen) {
          first_chunk_seen = true;
          first_chunk_ns = t0;
        }
        const uint64_t wall_us = static_cast<uint64_t>((t0 - first_chunk_ns) / 1000);
        const int64_t deficit_us = tools::deficitUs(wall_us, header.first_sample_index, options.rate_hz);
        deficit.update(deficit_us);
        if (timeline_stream.is_open()) {
          timeline_stream << tools::timelineRow(wall_us, header.first_sample_index, header.num_samples, deficit_us,
                                                checker != nullptr && checker->anyDeviation());
        }
      });
  if (!reader.has_value()) {
    stopGenerator(siggen, kAllPorts);
    return finishWith(reader.error());
  }
  auto checker = tools::PwmStreamChecker::create(tools::soakCheckConfig(options, *checked, (*reader)->captureId()));
  if (!checker.has_value()) {
    (*reader)->stop();
    stopGenerator(siggen, kAllPorts);
    return finishWith("cannot make checker: " + checker.error());
  }
  checker_for_reader.store(checker->get(), std::memory_order_release);

  bitoscope::host::pipe::CaptureRequest start;
  start.sample_rate_milli_hz = options.rate_hz * 1000;
  start.channel_mask = options.channel_mask;
  start.sample_limit = 0;
  start.mode = bitoscope::ipc::la::AcquisitionMode::streaming;
  start.capture_id = (*reader)->captureId();
  start.bulk_ring_name = (*reader)->name();
  std::printf("Starting capture...\n");
  const int64_t start_sent_ns = nowNs();
  if (auto started = la.start(start); !started) {
    (*reader)->stop();
    stopGenerator(siggen, kAllPorts);
    return finishWith("driver refused START: " + started.error());
  }
  const int64_t start_ns = nowNs();
  const auto run_start = Clock::now();
  const auto run_end = run_start + std::chrono::seconds(options.seconds);
  auto next_progress = Clock::now() + kProgressEvery;
  size_t next_change = 0;
  std::vector<std::string> change_log;
  bool change_failed = false;
  while (Clock::now() < run_end) {
    std::this_thread::sleep_for(next_change < options.changes.size() ? std::chrono::milliseconds(5) : kPoll);
    while (next_change < options.changes.size() &&
           Clock::now() - run_start >= std::chrono::milliseconds(options.changes[next_change].at_ms)) {
      const tools::PwmChange& change = options.changes[next_change++];
      const uint64_t before = samples_seen.load(std::memory_order_relaxed);
      const double sent_ms = secondsBetween(start_ns, nowNs()) * 1000.0;
      std::optional<std::string> error;
      if (change.signal) {
        error = startGenerator(siggen, change.port, *change.signal);
      } else if (auto stopped = siggen.stop(1u << change.port); !stopped) {
        error = "P" + std::to_string(change.port) + ": " + stopped.error();
      }
      const double answered_ms = secondsBetween(start_ns, nowNs()) * 1000.0;
      char line[256];
      std::snprintf(line, sizeof(line),
                    "at %.1f ms (asked for %llu ms) P%u -> %s: %s, answered at %.1f ms; stream had delivered "
                    "%llu samples when it was sent and %llu when it was answered",
                    sent_ms, static_cast<unsigned long long>(change.at_ms), change.port,
                    signalText(change.signal).c_str(), error ? ("REFUSED: " + *error).c_str() : "accepted",
                    answered_ms, static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(samples_seen.load(std::memory_order_relaxed)));
      std::printf("  %s\n", line);
      std::fflush(stdout);
      change_log.emplace_back(line);
      change_failed = change_failed || error.has_value();
    }
    if ((*reader)->end() != tools::RingReader::End::Open) {
      facts.ring_end_before_stop = true;
      break;
    }
    const uint64_t chunks = progress.chunks.load(std::memory_order_acquire);
    const int64_t now = nowNs();
    if (chunks == 0 && now - start_ns > std::chrono::nanoseconds(kFirstChunkTimeout).count()) {
      facts.stalled = true;
      std::fprintf(stderr, "  STALL: no chunk in %lld s after START\n", static_cast<long long>(kFirstChunkTimeout.count()));
      break;
    }
    if (chunks > 0 && now - progress.last_chunk_ns.load() > std::chrono::nanoseconds(kStallThreshold).count()) {
      facts.stalled = true;
      std::fprintf(stderr, "  STALL: no chunk for %.1f s\n", secondsBetween(progress.last_chunk_ns.load(), now));
      break;
    }
    if (Clock::now() >= next_progress) {
      next_progress += kProgressEvery;
      const double elapsed = secondsBetween(start_ns, now);
      const double mb = static_cast<double>(progress.bytes.load()) / 1e6;
      std::printf("  [%6.1f s] %llu chunks, %.1f MB, %.1f MB/s, ring peak %llu of %u slots, checker busy %.1f%%%s\n",
                  elapsed, static_cast<unsigned long long>(chunks), mb, elapsed > 0 ? mb / elapsed : 0.0,
                  static_cast<unsigned long long>((*reader)->peakSlotsInUse()), options.ringSlots(),
                  elapsed > 0 ? 100.0 * secondsBetween(0, progress.busy_ns.load()) / elapsed : 0.0,
                  (*checker)->anyDeviation() ? ", DEVIATION found" : "");
      std::fflush(stdout);
    }
  }

  (void)la.stop();
  stopGenerator(siggen, kAllPorts);
  const auto end_deadline = Clock::now() + kEndWait;
  while ((*reader)->end() == tools::RingReader::End::Open && Clock::now() < end_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  (*reader)->stop();
  if (timeline_stream.is_open()) timeline_stream.flush();

  facts.chunks = progress.chunks.load();
  facts.bytes = progress.bytes.load();
  const int64_t last = facts.chunks > 0 ? progress.last_chunk_ns.load() : start_ns;
  facts.wall_seconds = secondsBetween(start_ns, last);
  if (facts.chunks > 0) facts.first_chunk_ms = secondsBetween(start_sent_ns, first_chunk_ns) * 1000.0;
  facts.checker_busy_seconds = secondsBetween(0, progress.busy_ns.load());
  facts.peak_slots = (*reader)->peakSlotsInUse();
  facts.ring_end = ringEndOf((*reader)->end());
  if (auto record = (*reader)->failedRecord()) {
    facts.ring_error = record->reason;
    facts.ring_failure_kind = record->kind;
    facts.failed_at_sample = record->at_sample;
    facts.failed_lost_samples = record->lost_samples;
  } else if (facts.ring_end == tools::RingEnd::Invalid) {
    facts.ring_error = (*reader)->endReason();
  }
  report = (*checker)->report();
  if (!recorded.empty()) {
    std::printf("\n=== Generator during run ===\n");
    for (const auto& line : change_log) std::printf("  %s\n", line.c_str());
    for (size_t i = 0; i < recorded.size(); ++i) {
      const auto segments = recorders[i].segments();
      std::printf("  D%u (stream channel %u), %zu segment(s) of like runs (tolerance %u):\n", recorded[i].input,
                  recorded[i].stream_channel, segments.size(), options.run_tolerance);
      for (const auto& seg : segments) {
        const double high = seg.high_runs.mean();
        const double low = seg.low_runs.mean();
        const double period = high + low;
        std::printf("    samples %llu-%llu%s: %llu high run(s) %llu-%llu (mean %.3f), %llu low run(s) %llu-%llu (mean "
                    "%.3f)",
                    static_cast<unsigned long long>(seg.first_sample), static_cast<unsigned long long>(seg.end_sample),
                    seg.partial ? " (partial: stream's first or last run)" : "",
                    static_cast<unsigned long long>(seg.high_runs.count),
                    static_cast<unsigned long long>(seg.high_runs.min), static_cast<unsigned long long>(seg.high_runs.max),
                    high, static_cast<unsigned long long>(seg.low_runs.count),
                    static_cast<unsigned long long>(seg.low_runs.min), static_cast<unsigned long long>(seg.low_runs.max),
                    low);
        if (!seg.partial && seg.high_runs.count > 1 && seg.low_runs.count > 1) {
          std::printf("; period %.3f samples = %.3f Hz, duty %.2f%%", period,
                      static_cast<double>(options.rate_hz) / period, 100.0 * high / period);
        }
        std::printf("\n");
      }
    }
    std::printf("  Expected periods at %llu Hz (device's PWM clock model):\n",
                static_cast<unsigned long long>(options.rate_hz));
    for (uint32_t p = 0; p < tools::kRigPorts; ++p) {
      if (((options.changedPorts() >> p) & 1u) == 0) continue;
      std::vector<tools::PortSignal> signals;
      if (((options.off_ports >> p) & 1u) == 0) signals.push_back(options.ports[p]);
      for (const auto& change : options.changes) {
        if (change.port == p && change.signal) signals.push_back(*change.signal);
      }
      for (const auto& signal : signals) {
        const auto shape = tools::dlaPwmShape(options.rate_hz, signal.milli_hz, signal.duty_percent);
        std::printf("    P%u %s: %s\n", p, signalText(signal).c_str(),
                    shape ? ("period " + tools::formatSamples(shape->period_num, shape->den) + ", high " +
                             tools::formatSamples(shape->high_num, shape->den) + " samples")
                                .c_str()
                          : shape.error().c_str());
      }
    }
    if (change_failed) std::printf("  Generator command during run was REFUSED (above).\n");
  }
  return finishWith("");
}

}

int main(int argc, char** argv) {
  // WHY: Failure reasons carry a superscript two, so console must be UTF-8 while running and restored after.
  return bitoscope::console::runWithUtf8Console(bitoscope::console::systemConsoleCodePage(),
                                               [&] { return run(argc, argv); });
}

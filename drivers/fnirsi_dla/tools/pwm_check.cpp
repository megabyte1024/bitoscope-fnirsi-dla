/*
 * SPDX-License-Identifier: MIT
 */

#include "pwm_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <system_error>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace fnirsi::tools {

namespace {

std::optional<uint64_t> multiplied(uint64_t a, uint64_t b) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return std::nullopt;
  return a * b;
}

std::expected<uint64_t, std::string> decimal(std::string_view text) {
  uint64_t value = 0;
  if (text.empty() || text.front() < '0' || text.front() > '9') {
    return std::unexpected("'" + std::string(text) + "' is not whole number");
  }
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec == std::errc::result_out_of_range) {
    return std::unexpected("'" + std::string(text) + "' does not fit in 64 bits");
  }
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::unexpected("'" + std::string(text) + "' is not whole number");
  }
  return value;
}

}

std::expected<PwmShape, std::string> makePwmShape(uint64_t period_num, uint64_t period_den, uint32_t duty_percent) {
  if (period_num == 0) return std::unexpected(std::string("PWM period of zero samples"));
  if (period_den == 0) return std::unexpected(std::string("PWM period with zero denominator"));
  if (duty_percent < 1 || duty_percent > 99) {
    return std::unexpected("PWM duty of " + std::to_string(duty_percent) + "%: it must be 1 to 99");
  }
  const auto period = multiplied(period_num, 100);
  const auto high = multiplied(period_num, duty_percent);
  const auto den = multiplied(period_den, 100);
  if (!period || !high || !den) {
    return std::unexpected("PWM period of " + std::to_string(period_num) + "/" + std::to_string(period_den) +
                           " samples is too large to check exactly");
  }
  const uint64_t g = std::gcd(std::gcd(*period, *high), *den);
  PwmShape shape{*period / g, *high / g, *den / g};
  if (shape.period_num > kMaxShapeNum || shape.den > kMaxShapeDen) {
    return std::unexpected("PWM of " + formatSamples(shape.period_num, shape.den) +
                           " samples per period is beyond checker's exact arithmetic (numerator up to 2^62, "
                           "denominator up to 2^32)");
  }
  if (shape.high_num < shape.den || shape.lowNum() < shape.den) {
    return std::unexpected("PWM of " + formatSamples(shape.period_num, shape.den) + " samples at " +
                           std::to_string(duty_percent) + "% has run under one sample (high " +
                           formatSamples(shape.high_num, shape.den) + ", low " +
                           formatSamples(shape.lowNum(), shape.den) + ")");
  }
  return shape;
}

std::expected<PwmShape, std::string> pwmShapeFor(uint64_t sample_rate_hz, uint64_t pwm_milli_hz,
                                                 uint32_t duty_percent) {
  if (sample_rate_hz == 0) return std::unexpected(std::string("sample rate of 0 Hz"));
  if (pwm_milli_hz == 0) return std::unexpected(std::string("PWM frequency of 0 Hz"));
  const auto rate_milli_hz = multiplied(sample_rate_hz, 1000);
  if (!rate_milli_hz) return std::unexpected("sample rate of " + std::to_string(sample_rate_hz) + " Hz is too large");
  return makePwmShape(*rate_milli_hz, pwm_milli_hz, duty_percent);
}

std::expected<std::pair<uint64_t, uint64_t>, std::string> parseSamples(std::string_view text) {
  const size_t slash = text.find('/');
  const auto num = decimal(text.substr(0, slash));
  if (!num) return std::unexpected(num.error());
  if (slash == std::string_view::npos) return std::pair<uint64_t, uint64_t>{*num, 1};
  const auto den = decimal(text.substr(slash + 1));
  if (!den) return std::unexpected(den.error());
  if (*den == 0) return std::unexpected("'" + std::string(text) + "' divides by zero");
  return std::pair<uint64_t, uint64_t>{*num, *den};
}

std::string formatSamples(uint64_t num, uint64_t den) {
  if (den == 1) return std::to_string(num);
  char decimal_text[64];
  std::snprintf(decimal_text, sizeof(decimal_text), "%.3f", static_cast<double>(num) / static_cast<double>(den));
  return std::to_string(num) + "/" + std::to_string(den) + " (" + decimal_text + ")";
}

std::string describe(const Deviation& d, const PwmShape& shape, uint32_t edge_tolerance, uint32_t run_tolerance) {
  const std::string where = "channel " + std::to_string(d.channel) + " pulse " + std::to_string(d.pulse) + ": ";
  switch (d.kind) {
    case DeviationKind::EdgeOffPhase: {
      const std::string predicted = d.expected_first == d.expected_last
                                        ? "sample " + std::to_string(d.expected_first)
                                        : "samples " + std::to_string(d.expected_first) + "-" +
                                              std::to_string(d.expected_last);
      const bool early = d.got < d.expected_first;
      const uint64_t off = early ? d.expected_first - d.got : d.got - d.expected_last;
      return where + (d.level ? "rising" : "falling") + " edge at sample " + std::to_string(d.got) +
             ", predicted at " + predicted + " (+-" + std::to_string(edge_tolerance) + "): " + std::to_string(off) +
             " sample(s) " + (early ? "early" : "late");
    }
    case DeviationKind::RunLength: {
      const std::string level = d.level ? "high" : "low";
      const std::string expected =
          d.expected_first != 0 ? std::to_string(d.expected_first) + " +-" + std::to_string(run_tolerance) +
                                      " (channel's first " + level + " run)"
                                : formatSamples(d.level ? shape.high_num : shape.lowNum(), shape.den) + " +-" +
                                      std::to_string(run_tolerance);
      return where + level + " run of " + std::to_string(d.got) + " samples ending at sample " +
             std::to_string(d.sample) + ", expected " + expected;
    }
    case DeviationKind::NoEdge:
      return where + "no edge for more than one period (" + formatSamples(shape.period_num, shape.den) +
             " samples): " + (d.level ? "high" : "low") + " run reached " + std::to_string(d.got) +
             " samples at sample " + std::to_string(d.sample);
  }
  return where + "unknown deviation";
}

std::optional<Deviation> ChannelReport::firstDeviation() const {
  std::optional<Deviation> first;
  for (const auto* candidate : {&phase, &run, &no_edge}) {
    if (candidate->has_value() && (!first || (*candidate)->sample < first->sample)) first = **candidate;
  }
  return first;
}

double ChannelReport::measuredPeriod() const {
  if (rising_edges < 2) return 0.0;
  return static_cast<double>(last_rising - first_rising) / static_cast<double>(rising_edges - 1);
}

bool PwmCheckReport::clean() const {
  if (stream_error) return false;
  return std::all_of(channels.begin(), channels.end(),
                     [](const ChannelReport& c) { return !c.phase && !c.run && !c.no_edge; });
}

namespace {

static_assert(std::endian::native == std::endian::little,
              "byte lane i of word loaded from payload is bits 8i to 8i+7");

// WHY: tolerance * den must stay far inside 64 bits (den is at most 2^32).
constexpr uint32_t kMaxTolerance = 1'000'000;
constexpr int kSpins = 256;

int64_t floorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}
int64_t ceilDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && a > 0) ? q + 1 : q;
}

uint64_t load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void cpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  std::this_thread::yield();
#endif
}

// WHY: Phase arithmetic carries m * P as base_int + base_rem / den, so no large product can overflow.
class PhaseLock {
 public:
  explicit PhaseLock(const PwmShape& shape)
      : den_(static_cast<int64_t>(shape.den)),
        step_int_(shape.period_num / shape.den),
        step_rem_(static_cast<int64_t>(shape.period_num % shape.den)) {}

  bool locked() const { return locked_; }

  void lock(uint64_t sample) {
    locked_ = true;
    e0_ = sample;
    lo_ = -den_;
    hi_ = 0;
    base_int_ = step_int_;
    base_rem_ = step_rem_;
  }

  struct Prediction {
    uint64_t first = 0;
    uint64_t last = 0;
    int64_t off = 0;
  };

  Prediction predict(uint64_t sample) const {
    const int64_t emin = static_cast<int64_t>(base_int_) + floorDiv(lo_ + base_rem_, den_) + 1;
    const int64_t emax = static_cast<int64_t>(base_int_) + ceilDiv(hi_ + base_rem_, den_);
    const int64_t delta = static_cast<int64_t>(sample - e0_);
    const int64_t off = delta < emin ? delta - emin : (delta > emax ? delta - emax : 0);
    return Prediction{e0_ + static_cast<uint64_t>(emin), e0_ + static_cast<uint64_t>(emax), off};
  }

  void narrow(uint64_t sample) {
    const int64_t k = static_cast<int64_t>(sample - e0_) - static_cast<int64_t>(base_int_);
    lo_ = (std::max)(lo_, (k - 1) * den_ - base_rem_);
    hi_ = (std::min)(hi_, k * den_ - base_rem_);
  }

  void advance() {
    base_int_ += step_int_;
    base_rem_ += step_rem_;
    if (base_rem_ >= den_) {
      base_rem_ -= den_;
      ++base_int_;
    }
  }

 private:
  int64_t den_;
  uint64_t step_int_;
  int64_t step_rem_;
  bool locked_ = false;
  uint64_t e0_ = 0;
  int64_t lo_ = 0;
  int64_t hi_ = 0;
  uint64_t base_int_ = 0;
  int64_t base_rem_ = 0;
};

class ChannelTracker {
 public:
  ChannelTracker(const CheckedChannel& checked, uint32_t edge_tolerance, uint32_t run_tolerance,
                 RunReference run_reference, const EdgeObserver* observer, std::atomic<bool>* any_deviation)
      : rise_(checked.shape),
        fall_(checked.shape),
        observer_(observer),
        any_deviation_(any_deviation),
        edge_tolerance_(edge_tolerance),
        run_tolerance_(run_tolerance),
        learn_runs_(run_reference == RunReference::FirstRun),
        stuck_len_(checked.shape.period_num / checked.shape.den) {
    report_.channel = checked.channel;
    report_.shape = checked.shape;
    // WHY: Window bounds use integer division on num and den, never a product of a run, to avoid overflow.
    const int64_t den = static_cast<int64_t>(checked.shape.den);
    const int64_t tol = static_cast<int64_t>(run_tolerance) * den;
    const int64_t high = static_cast<int64_t>(checked.shape.high_num);
    const int64_t low = static_cast<int64_t>(checked.shape.lowNum());
    high_min_ = ceilDiv(high - tol, den);
    high_max_ = floorDiv(high + tol, den);
    low_min_ = ceilDiv(low - tol, den);
    low_max_ = floorDiv(low + tol, den);
  }

  void edge(uint64_t sample, bool level_after) {
    if (observer_ != nullptr && *observer_) (*observer_)(report_.channel, sample, level_after);
    const bool run_level = !level_after;
    const uint64_t run = sample - run_start_;
    const uint64_t pulse = report_.edges;
    if (!report_.no_edge && run > stuck_len_) {
      record(report_.no_edge, Deviation{DeviationKind::NoEdge, report_.channel, pulse, run_start_ + stuck_len_,
                                        run_level, 0, 0, stuck_len_ + 1});
    }
    if (have_edge_) {
      RunStats& stats = run_level ? report_.high_runs : report_.low_runs;
      stats.min = stats.count == 0 ? run : (std::min)(stats.min, run);
      stats.max = (std::max)(stats.max, run);
      stats.sum += run;
      ++stats.count;
      std::optional<uint64_t>& first = run_level ? first_high_ : first_low_;
      if (learn_runs_ && !first) {
        first = run;
        const int64_t r = static_cast<int64_t>(run);
        const int64_t tol = static_cast<int64_t>(run_tolerance_);
        (run_level ? high_min_ : low_min_) = r - tol;
        (run_level ? high_max_ : low_max_) = r + tol;
      }
      const int64_t lo = run_level ? high_min_ : low_min_;
      const int64_t hi = run_level ? high_max_ : low_max_;
      if (!report_.run && (static_cast<int64_t>(run) < lo || static_cast<int64_t>(run) > hi)) {
        const uint64_t reference = learn_runs_ ? *first : 0;
        record(report_.run, Deviation{DeviationKind::RunLength, report_.channel, pulse, sample, run_level, reference,
                                      reference, run});
      }
    }
    PhaseLock& lock = level_after ? rise_ : fall_;
    if (!lock.locked()) {
      lock.lock(sample);
    } else {
      const PhaseLock::Prediction p = lock.predict(sample);
      const uint64_t off = static_cast<uint64_t>(p.off < 0 ? -p.off : p.off);
      report_.last_phase_error = p.off;
      report_.max_phase_error = (std::max)(report_.max_phase_error, off);
      if (!report_.phase) {
        if (off > edge_tolerance_) {
          record(report_.phase, Deviation{DeviationKind::EdgeOffPhase, report_.channel, pulse, sample, level_after,
                                          p.first, p.last, sample});
        } else {
          ++report_.edges_phase_checked;
          if (off == 0) {
            ++report_.edges_exact;
            lock.narrow(sample);
          } else if (off == 1) {
            ++report_.edges_off_by_one;
          }
        }
      }
      lock.advance();
    }
    if (level_after) {
      if (report_.rising_edges == 0) report_.first_rising = sample;
      report_.last_rising = sample;
      ++report_.rising_edges;
    }
    ++report_.edges;
    run_start_ = sample;
    have_edge_ = true;
  }

  void endOfData(uint64_t end, bool level) {
    if (!report_.no_edge && end - run_start_ > stuck_len_) {
      record(report_.no_edge, Deviation{DeviationKind::NoEdge, report_.channel, report_.edges,
                                        run_start_ + stuck_len_, level, 0, 0, stuck_len_ + 1});
    }
  }

  const ChannelReport& report() const { return report_; }

 private:
  void record(std::optional<Deviation>& slot, const Deviation& deviation) {
    slot = deviation;
    any_deviation_->store(true, std::memory_order_relaxed);
  }

  PhaseLock rise_;
  PhaseLock fall_;
  const EdgeObserver* observer_;
  std::atomic<bool>* any_deviation_;
  uint64_t edge_tolerance_;
  uint64_t run_tolerance_;
  bool learn_runs_;
  std::optional<uint64_t> first_high_;
  std::optional<uint64_t> first_low_;
  uint64_t stuck_len_;
  int64_t high_min_ = 0;
  int64_t high_max_ = 0;
  int64_t low_min_ = 0;
  int64_t low_max_ = 0;
  uint64_t run_start_ = 0;
  bool have_edge_ = false;
  ChannelReport report_;
};

class GroupScanner {
 public:
  explicit GroupScanner(uint32_t num_channels)
      : n_(num_channels),
        by_word_(8 % num_channels == 0 || num_channels == 16),
        block_bytes_((std::max)(num_channels, 8u)),
        words_(block_bytes_ / 8),
        bytes_per_lane_(block_bytes_ / num_channels),
        samples_per_block_(bytes_per_lane_ * 8) {}

  void add(ChannelTracker* tracker, uint32_t channel) {
    Lane lane;
    lane.tracker = tracker;
    lane.channel = channel;
    lane.word = channel / 8;
    lane.first_lane = channel % 8;
    if (by_word_) {
      for (uint32_t j = 0; j < bytes_per_lane_; ++j) lane.mask |= uint64_t{0xFF} << (8 * (lane.first_lane + j * n_));
      mask_[lane.word] |= lane.mask;
    }
    lanes_.push_back(lane);
  }

  void scan(uint64_t first_sample, const uint8_t* data, size_t bytes) {
    if (bytes == 0) return;
    if (!started_) {
      for (Lane& lane : lanes_) lane.level = (data[lane.channel] & 0x01u) != 0;
      for (uint32_t w = 0; w < words_; ++w) level_[w] = levelWord(w);
      started_ = true;
    }
    if (!by_word_) {
      scanRounds(first_sample, data, bytes / n_);
      const uint64_t end = first_sample + bytes / n_ * 8;
      for (Lane& lane : lanes_) lane.tracker->endOfData(end, lane.level);
      return;
    }
    const size_t blocks = bytes / block_bytes_;
    for (uint32_t w = 0; w < words_; ++w) {
      if (mask_[w] != 0) level_[w] = scanWords(w, data + 8 * w, blocks, first_sample);
    }
    const size_t done = blocks * block_bytes_;
    const size_t rounds = (bytes - done) / n_;
    const uint64_t tail_first = first_sample + blocks * samples_per_block_;
    for (size_t r = 0; r < rounds; ++r) {
      for (Lane& lane : lanes_) processByte(lane, data[done + r * n_ + lane.channel], tail_first + r * 8);
    }
    for (uint32_t w = 0; w < words_; ++w) level_[w] = levelWord(w);
    const uint64_t end = first_sample + bytes / n_ * 8;
    for (Lane& lane : lanes_) lane.tracker->endOfData(end, lane.level);
  }

 private:
  struct Lane {
    ChannelTracker* tracker = nullptr;
    uint32_t channel = 0;
    uint32_t word = 0;
    uint32_t first_lane = 0;
    uint64_t mask = 0;
    bool level = false;
  };

  void scanRounds(uint64_t first_sample, const uint8_t* data, size_t rounds) {
    for (size_t r = 0; r < rounds; ++r) {
      const uint8_t* round = data + r * n_;
      for (Lane& lane : lanes_) {
        const uint32_t byte = round[lane.channel];
        if (byte != (lane.level ? 0xFFu : 0u)) processByte(lane, byte, first_sample + r * 8);
      }
    }
  }

  uint64_t levelWord(uint32_t w) const {
    uint64_t word = 0;
    for (const Lane& lane : lanes_) {
      if (lane.word == w && lane.level) word |= lane.mask;
    }
    return word;
  }

  uint64_t scanWords(uint32_t w, const uint8_t* base, size_t blocks, uint64_t first_sample) {
    uint64_t level = level_[w];
    const uint64_t mask = mask_[w];
    const size_t stride = block_bytes_;
    size_t b = 0;
    for (; b + 4 <= blocks; b += 4) {
      const uint8_t* q = base + b * stride;
      const uint64_t w0 = load64(q);
      const uint64_t w1 = load64(q + stride);
      const uint64_t w2 = load64(q + 2 * stride);
      const uint64_t w3 = load64(q + 3 * stride);
      if ((((w0 ^ level) | (w1 ^ level) | (w2 ^ level) | (w3 ^ level)) & mask) == 0) continue;
      const uint64_t four[4] = {w0, w1, w2, w3};
      for (size_t i = 0; i < 4; ++i) {
        if (((four[i] ^ level) & mask) != 0) {
          level = transitions(w, four[i], first_sample + (b + i) * samples_per_block_);
        }
      }
    }
    for (; b < blocks; ++b) {
      const uint64_t word = load64(base + b * stride);
      if (((word ^ level) & mask) != 0) level = transitions(w, word, first_sample + b * samples_per_block_);
    }
    return level;
  }

  uint64_t transitions(uint32_t w, uint64_t word, uint64_t block_first_sample) {
    for (Lane& lane : lanes_) {
      if (lane.word != w) continue;
      const uint64_t expected = lane.level ? lane.mask : 0;
      if (((word ^ expected) & lane.mask) == 0) continue;
      for (uint32_t j = 0; j < bytes_per_lane_; ++j) {
        const auto byte = static_cast<uint32_t>((word >> (8 * (lane.first_lane + j * n_))) & 0xFFu);
        processByte(lane, byte, block_first_sample + j * 8);
      }
    }
    return levelWord(w);
  }

  static void processByte(Lane& lane, uint32_t byte, uint64_t first_sample) {
    uint32_t current = lane.level ? 0xFFu : 0u;
    uint32_t diff = byte ^ current;
    while (diff != 0) {
      const int pos = std::countr_zero(static_cast<uint8_t>(diff));
      lane.level = !lane.level;
      lane.tracker->edge(first_sample + static_cast<uint64_t>(pos), lane.level);
      current ^= 0xFFu;
      diff = (byte ^ current) & (0xFFu << (pos + 1));
    }
  }

  uint32_t n_;
  bool by_word_;
  uint32_t block_bytes_;
  uint32_t words_;
  uint32_t bytes_per_lane_;
  uint32_t samples_per_block_;
  std::vector<Lane> lanes_;
  std::array<uint64_t, 2> mask_{};
  std::array<uint64_t, 2> level_{};
  bool started_ = false;
};

bool supportedChannelCount(uint32_t n) { return n >= 1 && n <= 16; }

}

struct PwmStreamChecker::Impl {
  PwmCheckConfig config;
  std::atomic<bool> any_deviation{false};
  std::vector<std::unique_ptr<ChannelTracker>> trackers;
  std::vector<std::unique_ptr<GroupScanner>> groups;

  uint64_t chunks = 0;
  uint64_t payload_bytes = 0;
  uint64_t next_sample = 0;
  std::optional<StreamError> stream_error;

  // WHY: Workers read job_* after acquiring new generation; last worker's pending == 0 publishes trackers.
  std::vector<std::thread> threads;
  std::atomic<uint64_t> generation{0};
  std::atomic<uint32_t> pending{0};
  bool stopping = false;
  const uint8_t* job_data = nullptr;
  size_t job_bytes = 0;
  uint64_t job_first = 0;

  ~Impl() { stopWorkers(); }

  void stopWorkers() {
    if (threads.empty()) return;
    stopping = true;
    generation.fetch_add(1, std::memory_order_release);
    generation.notify_all();
    for (auto& t : threads) t.join();
    threads.clear();
  }

  void work(size_t group) {
    uint64_t seen = 0;
    for (;;) {
      uint64_t now = generation.load(std::memory_order_acquire);
      for (int i = 0; now == seen && i < kSpins; ++i) {
        cpuRelax();
        now = generation.load(std::memory_order_acquire);
      }
      while (now == seen) {
        generation.wait(seen, std::memory_order_acquire);
        now = generation.load(std::memory_order_acquire);
      }
      seen = now;
      if (stopping) return;
      groups[group]->scan(job_first, job_data, job_bytes);
      if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) pending.notify_one();
    }
  }

  void scanAll(uint64_t first_sample, const uint8_t* data, size_t bytes) {
    if (threads.empty()) {
      groups[0]->scan(first_sample, data, bytes);
      return;
    }
    job_first = first_sample;
    job_data = data;
    job_bytes = bytes;
    pending.store(static_cast<uint32_t>(threads.size()), std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
    generation.notify_all();
    uint32_t left = pending.load(std::memory_order_acquire);
    for (int i = 0; left != 0 && i < kSpins; ++i) {
      cpuRelax();
      left = pending.load(std::memory_order_acquire);
    }
    while (left != 0) {
      pending.wait(left, std::memory_order_acquire);
      left = pending.load(std::memory_order_acquire);
    }
  }

  std::optional<StreamError> checkHeader(const bitoscope::bulk_ring::ChunkHeader& h, size_t payload_size) const {
    const auto bad = [&](const std::string& message) {
      return StreamError{StreamError::Kind::BadHeader, chunks, next_sample, h.first_sample_index,
                         "chunk " + std::to_string(chunks) + ": " + message};
    };
    const uint32_t n = config.num_channels;
    if (h.num_channels != n) {
      return bad("it carries " + std::to_string(h.num_channels) + " channels, check expects " + std::to_string(n));
    }
    if (h.encoding != kChannelMajorPackedEncoding) {
      return bad("encoding " + std::to_string(h.encoding) + ", not CHANNEL_MAJOR_PACKED (" +
                 std::to_string(kChannelMajorPackedEncoding) + ")");
    }
    if (h.sample_rate_milli_hz != config.sample_rate_milli_hz) {
      return bad("rate of " + std::to_string(h.sample_rate_milli_hz) + " mHz, check expects " +
                 std::to_string(config.sample_rate_milli_hz));
    }
    if (config.capture_id && h.capture_id != *config.capture_id) {
      return bad("capture id " + std::to_string(h.capture_id) + ", check expects " +
                 std::to_string(*config.capture_id));
    }
    if (h.payload_bytes != payload_size) {
      return bad("header says " + std::to_string(h.payload_bytes) + " payload bytes, slot holds " +
                 std::to_string(payload_size));
    }
    if (payload_size % n != 0) {
      return bad(std::to_string(payload_size) + " payload bytes are not whole rounds of " + std::to_string(n) +
                 " channels");
    }
    if (h.num_samples != payload_size / n * 8) {
      return bad("header says " + std::to_string(h.num_samples) + " samples per channel, payload holds " +
                 std::to_string(payload_size / n * 8));
    }
    if (h.first_sample_index != next_sample) {
      const bool gap = h.first_sample_index > next_sample;
      const uint64_t by = gap ? h.first_sample_index - next_sample : next_sample - h.first_sample_index;
      return StreamError{StreamError::Kind::Discontinuity, chunks, next_sample, h.first_sample_index,
                         "chunk " + std::to_string(chunks) + " starts at sample " +
                             std::to_string(h.first_sample_index) + " where sample " + std::to_string(next_sample) +
                             " was due: " + std::to_string(by) + " samples " + (gap ? "missing" : "repeated")};
    }
    return std::nullopt;
  }
};

std::expected<std::unique_ptr<PwmStreamChecker>, std::string> PwmStreamChecker::create(PwmCheckConfig config) {
  const uint32_t n = config.num_channels;
  if (!supportedChannelCount(n)) {
    return std::unexpected("stream of " + std::to_string(n) + " channels: check reads 1 to 16");
  }
  if (config.sample_rate_milli_hz == 0) return std::unexpected(std::string("sample rate of 0"));
  if (config.channels.empty()) return std::unexpected(std::string("no channel to check"));
  if (config.edge_tolerance > kMaxTolerance || config.run_tolerance > kMaxTolerance) {
    return std::unexpected("tolerance over " + std::to_string(kMaxTolerance) + " samples");
  }
  std::vector<bool> taken(n, false);
  for (const auto& c : config.channels) {
    if (c.channel >= n) {
      return std::unexpected("channel " + std::to_string(c.channel) + " is not in stream of " + std::to_string(n));
    }
    if (taken[c.channel]) return std::unexpected("channel " + std::to_string(c.channel) + " is given twice");
    taken[c.channel] = true;
    const PwmShape& s = c.shape;
    if (s.den == 0 || s.period_num == 0 || s.den > kMaxShapeDen || s.period_num > kMaxShapeNum ||
        s.high_num < s.den || s.high_num > s.period_num || s.lowNum() < s.den) {
      return std::unexpected("channel " + std::to_string(c.channel) +
                             ": not PWM shape that check can use (zero, run under one sample, or past its exact "
                             "arithmetic); make it with makePwmShape()");
    }
  }
  if (config.workers > config.channels.size()) {
    return std::unexpected(std::to_string(config.workers) + " workers for " + std::to_string(config.channels.size()) +
                           " channels: at most one worker per channel");
  }

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  Impl& im = *impl;
  const size_t group_count = (std::max)(im.config.workers, 1u);
  for (size_t g = 0; g < group_count; ++g) im.groups.push_back(std::make_unique<GroupScanner>(n));
  for (size_t i = 0; i < im.config.channels.size(); ++i) {
    im.trackers.push_back(std::make_unique<ChannelTracker>(im.config.channels[i], im.config.edge_tolerance,
                                                           im.config.run_tolerance, im.config.run_reference,
                                                           &im.config.on_edge, &im.any_deviation));
    im.groups[i % group_count]->add(im.trackers.back().get(), im.config.channels[i].channel);
  }
  try {
    for (uint32_t w = 0; w < im.config.workers; ++w) im.threads.emplace_back([&im, w] { im.work(w); });
  } catch (const std::system_error& e) {
    im.stopWorkers();
    return std::unexpected("cannot start checker's worker threads: " + std::string(e.what()));
  }
  return std::unique_ptr<PwmStreamChecker>(new PwmStreamChecker(std::move(impl)));
}

PwmStreamChecker::PwmStreamChecker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PwmStreamChecker::~PwmStreamChecker() = default;

bool PwmStreamChecker::checkChunk(const bitoscope::bulk_ring::ChunkHeader& header, std::span<const uint8_t> payload) {
  Impl& im = *impl_;
  if (im.stream_error) return false;
  if (auto error = im.checkHeader(header, payload.size())) {
    im.stream_error = std::move(error);
    im.any_deviation.store(true, std::memory_order_relaxed);
    return false;
  }
  if (!payload.empty()) im.scanAll(header.first_sample_index, payload.data(), payload.size());
  ++im.chunks;
  im.payload_bytes += payload.size();
  im.next_sample += header.num_samples;
  return true;
}

bool PwmStreamChecker::anyDeviation() const { return impl_->any_deviation.load(std::memory_order_relaxed); }

PwmCheckReport PwmStreamChecker::report() const {
  const Impl& im = *impl_;
  PwmCheckReport report;
  report.chunks = im.chunks;
  report.payload_bytes = im.payload_bytes;
  report.samples_per_channel = im.next_sample;
  report.stream_error = im.stream_error;
  report.edge_tolerance = im.config.edge_tolerance;
  report.run_tolerance = im.config.run_tolerance;
  for (const auto& tracker : im.trackers) report.channels.push_back(tracker->report());
  return report;
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace bitoscope::bulk_ring {

inline constexpr uint64_t kMagic = 0x31474E4952535442ull;
inline constexpr uint32_t kLayoutVersion = 5;

inline constexpr size_t kCacheLineBytes = 64;
inline constexpr size_t kPayloadAlignment = 64;

// WHY: kMaxNameLength is macOS's PSHMNAMLEN (31); whether macOS counts shm_open's leading slash was not checked.
inline constexpr size_t kMaxNameLength = 31;

inline constexpr size_t kReasonBytes = 92;

inline constexpr uint64_t kDefaultStaleAfterMs = 5000;

inline constexpr uint64_t kMaxWakeWaitMs = 50;

// WHY: 8 ms: under half a frame of a 60 Hz live view; 64 slots x 8 ms still outlast the ~30 ms a woken host
// waits for a core on a busy PC, and at 5 Gbit/s a 4 MiB slot fills in 6.7 ms.
inline constexpr uint32_t kDefaultFlushAfterMs = 8;

enum class ProducerState : uint32_t { Attached = 0, Ended = 1, Failed = 2 };

// WHY: FailureKind is not range-checked: a reader takes a kind it does not know as Unspecified, so kinds can be
// added without a layout version. A kind's number is never reused.
enum class FailureKind : uint32_t {
  Unspecified = 0,
  RingFull = 1,
  ConsumerStale = 2,
};
enum class ConsumerState : uint32_t { Reading = 0, Closed = 1 };

enum class CreatorRole {
  Consumer,
  Producer,
};

namespace layout {

inline constexpr uint32_t kMaxChunksPerSlot = 256;

struct RingInfo {
  uint64_t magic;
  uint32_t layout_version;
  uint32_t slot_count;
  uint64_t slot_payload_bytes;
  uint64_t capture_id;
  uint32_t flush_after_ms;
  uint8_t reserved[28];
};

struct ProducerLine {
  uint64_t produced;
  uint32_t producer_state;
  uint32_t producer_waiting;
  uint64_t failed_at_sample;
  uint64_t lost_samples;
  uint32_t failure_kind;
  char reason[kReasonBytes];
};

struct ConsumerLine {
  uint64_t consumed;
  uint32_t consumer_state;
  uint32_t consumer_waiting;
  uint64_t consumer_heartbeat_ms;
  uint8_t reserved[40];
};

struct ControlBlock {
  RingInfo info;
  ProducerLine producer;
  ConsumerLine consumer;
};

struct SlotHeader {
  uint32_t chunk_count;
  uint32_t reserved0;
  uint64_t used_bytes;
  uint8_t reserved[48];
};

struct ChunkRecord {
  uint64_t first_sample_index;
  uint64_t num_samples;
  uint32_t num_channels;
  uint32_t encoding;
  uint64_t sample_rate_milli_hz;
  uint64_t capture_id;
  uint64_t payload_bytes;
  uint8_t reserved[16];
};

static_assert(std::endian::native == std::endian::little,
              "the ring's layout is little-endian and is read in place");

static_assert(sizeof(RingInfo) == kCacheLineBytes);
static_assert(sizeof(ProducerLine) == 2 * kCacheLineBytes);
static_assert(sizeof(ConsumerLine) == kCacheLineBytes);
static_assert(sizeof(ControlBlock) == 4 * kCacheLineBytes);
static_assert(sizeof(SlotHeader) == kCacheLineBytes);
static_assert(sizeof(ChunkRecord) == kPayloadAlignment);

static_assert(offsetof(ControlBlock, info) == 0);
static_assert(offsetof(ControlBlock, producer) == 1 * kCacheLineBytes);
static_assert(offsetof(ControlBlock, consumer) == 3 * kCacheLineBytes);

static_assert(offsetof(RingInfo, magic) == 0);
static_assert(offsetof(RingInfo, layout_version) == 8);
static_assert(offsetof(RingInfo, slot_count) == 12);
static_assert(offsetof(RingInfo, slot_payload_bytes) == 16);
static_assert(offsetof(RingInfo, capture_id) == 24);
static_assert(offsetof(RingInfo, flush_after_ms) == 32);

static_assert(offsetof(ProducerLine, produced) == 0);
static_assert(offsetof(ProducerLine, producer_state) == 8);
static_assert(offsetof(ProducerLine, producer_waiting) == 12);
static_assert(offsetof(ProducerLine, failed_at_sample) == 16);
static_assert(offsetof(ProducerLine, lost_samples) == 24);
static_assert(offsetof(ProducerLine, failure_kind) == 32);
static_assert(offsetof(ProducerLine, reason) == 36);

static_assert(offsetof(ConsumerLine, consumed) == 0);
static_assert(offsetof(ConsumerLine, consumer_state) == 8);
static_assert(offsetof(ConsumerLine, consumer_waiting) == 12);
static_assert(offsetof(ConsumerLine, consumer_heartbeat_ms) == 16);

static_assert(offsetof(SlotHeader, chunk_count) == 0);
static_assert(offsetof(SlotHeader, used_bytes) == 8);

static_assert(offsetof(ChunkRecord, first_sample_index) == 0);
static_assert(offsetof(ChunkRecord, num_samples) == 8);
static_assert(offsetof(ChunkRecord, num_channels) == 16);
static_assert(offsetof(ChunkRecord, encoding) == 20);
static_assert(offsetof(ChunkRecord, sample_rate_milli_hz) == 24);
static_assert(offsetof(ChunkRecord, capture_id) == 32);
static_assert(offsetof(ChunkRecord, payload_bytes) == 40);

// WHY: Atomics on shared memory must be lock-free: a locking one takes a process-local lock and does not synchronise.
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
static_assert(offsetof(ControlBlock, producer) % std::atomic_ref<uint64_t>::required_alignment == 0);
static_assert(offsetof(ControlBlock, consumer) % std::atomic_ref<uint64_t>::required_alignment == 0);
static_assert(offsetof(ProducerLine, producer_state) % std::atomic_ref<uint32_t>::required_alignment == 0);
static_assert(offsetof(ProducerLine, producer_waiting) % std::atomic_ref<uint32_t>::required_alignment == 0);
static_assert(offsetof(ConsumerLine, consumer_state) % std::atomic_ref<uint32_t>::required_alignment == 0);
static_assert(offsetof(ConsumerLine, consumer_waiting) % std::atomic_ref<uint32_t>::required_alignment == 0);
static_assert(offsetof(ConsumerLine, consumer_heartbeat_ms) % std::atomic_ref<uint64_t>::required_alignment == 0);

// WHY: Caller keeps bytes below 2^64 - 63: rounding up would wrap.
constexpr uint64_t padded(uint64_t bytes) {
  return (bytes + kPayloadAlignment - 1) / kPayloadAlignment * kPayloadAlignment;
}

constexpr uint64_t chunkBytes(uint64_t payload_bytes) { return sizeof(ChunkRecord) + padded(payload_bytes); }

inline constexpr uint64_t kSlotFixedBytes = sizeof(SlotHeader) + uint64_t{kMaxChunksPerSlot} * sizeof(ChunkRecord);

constexpr uint64_t chunkAreaBytes(uint64_t slot_payload_bytes) {
  return uint64_t{kMaxChunksPerSlot} * sizeof(ChunkRecord) + padded(slot_payload_bytes);
}

constexpr uint64_t slotStride(uint64_t slot_payload_bytes) { return kSlotFixedBytes + padded(slot_payload_bytes); }

constexpr uint64_t slotOffset(uint64_t slot_payload_bytes, uint64_t slot) {
  return sizeof(ControlBlock) + slot * slotStride(slot_payload_bytes);
}

constexpr uint64_t chunkAreaOffset(uint64_t slot_payload_bytes, uint64_t slot) {
  return slotOffset(slot_payload_bytes, slot) + sizeof(SlotHeader);
}

constexpr uint64_t firstPayloadOffset(uint64_t slot_payload_bytes, uint64_t slot) {
  return chunkAreaOffset(slot_payload_bytes, slot) + sizeof(ChunkRecord);
}

static_assert(kSlotFixedBytes == 64 + 256 * 64);
static_assert(chunkBytes(0) == 64);
static_assert(chunkBytes(1) == 128);
static_assert(chunkBytes(64) == 128);
static_assert(chunkBytes(65) == 192);
static_assert(slotStride(64) == kSlotFixedBytes + 64);
static_assert(slotStride(65) == kSlotFixedBytes + 128);
static_assert(chunkAreaBytes(100) == slotStride(100) - sizeof(SlotHeader));
static_assert(firstPayloadOffset(100, 0) % kPayloadAlignment == 0);
static_assert(firstPayloadOffset(100, 3) % kPayloadAlignment == 0);

}

struct ChunkHeader {
  uint64_t first_sample_index = 0;
  uint64_t num_samples = 0;
  uint32_t num_channels = 0;
  uint32_t encoding = 0;
  uint64_t sample_rate_milli_hz = 0;
  uint64_t capture_id = 0;
  uint64_t payload_bytes = 0;

  bool operator==(const ChunkHeader&) const = default;
};

struct FailedRecord {
  uint64_t at_sample = 0;
  uint64_t lost_samples = 0;
  std::string reason;
  FailureKind kind = FailureKind::Unspecified;

  bool operator==(const FailedRecord&) const = default;
};

enum class AcquireErrorKind {
  TooLarge,
  Full,
  Timeout,
  ConsumerClosed,
  ConsumerStale,
  ConsumerInvalid,
  ProducerFinished,
};

struct AcquireError {
  AcquireErrorKind kind = AcquireErrorKind::Full;
  std::string message;
};

struct ReadChunk {
  ChunkHeader header;
  std::span<const uint8_t> payload;
};

enum class PollStatus {
  Chunk,
  Empty,
  Ended,
  Failed,
  Invalid,
};

struct Poll {
  PollStatus status = PollStatus::Empty;
  ReadChunk chunk;
  std::string error;
};

struct Timing {
  std::function<uint64_t()> now_ms;
  uint64_t stale_after_ms = kDefaultStaleAfterMs;
};

std::string makeRingName();

std::string wakeObjectName(std::string_view ring_name);

std::string producerWakeObjectName(std::string_view ring_name);

class BulkRing {
 public:
  static std::expected<BulkRing, std::string> create(std::string_view name, uint32_t slot_count,
                                                     uint64_t slot_payload_bytes, uint64_t capture_id,
                                                     Timing timing = {},
                                                     uint32_t flush_after_ms = kDefaultFlushAfterMs,
                                                     CreatorRole creator_role = CreatorRole::Consumer);

  static std::expected<BulkRing, std::string> open(std::string_view name, Timing timing = {});

  BulkRing(BulkRing&& other) noexcept;
  BulkRing& operator=(BulkRing&& other) noexcept;
  BulkRing(const BulkRing&) = delete;
  BulkRing& operator=(const BulkRing&) = delete;
  ~BulkRing();

  const std::string& name() const { return name_; }
  uint32_t slotCount() const { return slot_count_; }
  uint64_t slotPayloadBytes() const { return slot_payload_bytes_; }
  uint64_t captureId() const { return capture_id_; }
  uint32_t flushAfterMs() const { return flush_after_ms_; }
  size_t mappingBytes() const { return mapping_bytes_; }

  uint64_t nowMs() const;

  uint64_t produced() const;
  uint64_t consumed() const;
  ProducerState producerState() const;
  ConsumerState consumerState() const;
  uint64_t consumerHeartbeatMs() const;
  FailedRecord failedRecord() const;

  std::expected<std::span<uint8_t>, AcquireError> tryAcquire(uint64_t max_bytes);

  std::expected<std::span<uint8_t>, AcquireError> acquire(uint64_t max_bytes, uint64_t deadline_ms);

  std::expected<void, std::string> commit(const ChunkHeader& header);

  void flush();

  bool flushIfDue();

  std::expected<void, std::string> end();

  std::expected<void, std::string> markFailed(uint64_t at_sample, uint64_t lost_samples,
                                              std::string_view reason,
                                              FailureKind kind = FailureKind::Unspecified);

  Poll peek();

  Poll wait(uint64_t deadline_ms);

  std::expected<void, std::string> release();

  std::expected<void, std::string> advance();

  std::expected<void, std::string> releaseSlotsBefore(uint64_t slot);

  uint64_t readingSlot() const;
  uint64_t retainedSlots() const { return reading_.ahead; }

  void close();

  void heartbeat();

 private:
  friend struct BulkRingTestAccess;

  BulkRing(std::string name, uint8_t* base, size_t mapping_bytes, void* native_handle, bool owner,
           Timing timing);
  void releaseMapping() noexcept;
  void abandonProduction() noexcept;
  uint64_t endOfLastCommit() const;

  layout::ControlBlock& control() const;
  layout::SlotHeader& slotHeader(uint64_t index) const;
  uint8_t* chunkArea(uint64_t index) const;
  AcquireError finishedError() const;

  bool fits(uint64_t max_bytes) const;
  void commitSlot();
  void wakeConsumer();
  void blockUntilWoken(uint64_t timeout_ms);
  void wakeProducer();
  void blockUntilRoom(uint64_t timeout_ms);

  std::string name_;
  uint8_t* base_ = nullptr;
  size_t mapping_bytes_ = 0;
  void* native_handle_ = nullptr;
  void* wake_ = nullptr;
  std::string wake_name_;
  void* producer_wake_ = nullptr;
  std::string producer_wake_name_;
  bool owner_ = false;
  CreatorRole creator_role_ = CreatorRole::Consumer;
  Timing timing_;

  // WHY: Geometry is read once and never re-read from the mapping: the other process could change it after a check.
  uint32_t slot_count_ = 0;
  uint64_t slot_payload_bytes_ = 0;
  uint64_t capture_id_ = 0;
  uint32_t flush_after_ms_ = kDefaultFlushAfterMs;

  struct Filling {
    bool open = false;
    uint32_t chunks = 0;
    uint64_t payload_bytes = 0;
    uint64_t first_ms = 0;
    bool held = false;
    uint64_t reserved_bytes = 0;
    bool committed_any = false;
    uint64_t last_commit_ms = 0;
    uint64_t seen_consumed = 0;
  };
  struct Reading {
    uint64_t ahead = 0;
    uint32_t chunks = 0;
    uint64_t offset = 0;
    bool peeked = false;
    uint64_t peeked_end = 0;
    bool peeked_last = false;
  };
  Filling filling_;
  Reading reading_;

  // WHY: TEST ONLY: tests pin a woken wait by counting, because a wall-clock bound on a loaded scheduler
  // measures the scheduler. `lost` stays 0 however loaded the machine is.
  struct WaitCounters {
    uint64_t blocks = 0;
    uint64_t timeouts = 0;
    uint64_t lost = 0;
  };
  WaitCounters consumer_waits_;
  WaitCounters producer_waits_;
};

}

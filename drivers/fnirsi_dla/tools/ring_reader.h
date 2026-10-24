/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "bulk_ring.h"

namespace fnirsi::tools {

class RingReader {
 public:
  using ChunkFn = std::function<void(const bitoscope::bulk_ring::ChunkHeader& header,
                                     std::span<const uint8_t> payload)>;

  static std::expected<std::unique_ptr<RingReader>, std::string> start(uint32_t slot_count,
                                                                       uint64_t slot_payload_bytes,
                                                                       ChunkFn on_chunk);

  ~RingReader();

  RingReader(const RingReader&) = delete;
  RingReader& operator=(const RingReader&) = delete;

  const std::string& name() const { return ring_.name(); }
  uint64_t captureId() const { return ring_.captureId(); }

  void stop();

  std::string endReason() const;
  bool failed() const { return failed_.load(); }

  enum class End {
    Open,
    Ended,
    Failed,
    Invalid,
  };
  End end() const;
  std::optional<bitoscope::bulk_ring::FailedRecord> failedRecord() const;
  uint64_t peakSlotsInUse() const { return peak_.load(); }

 private:
  RingReader(bitoscope::bulk_ring::BulkRing ring, ChunkFn on_chunk);
  void run();

  bitoscope::bulk_ring::BulkRing ring_;
  const ChunkFn on_chunk_;
  mutable std::mutex mutex_;
  std::string end_reason_;
  End end_ = End::Open;
  std::optional<bitoscope::bulk_ring::FailedRecord> failed_record_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> peak_{0};
  std::thread thread_;  // WHY: thread_ is declared last so everything it uses exists before it starts.
};

}

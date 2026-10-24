/*
 * SPDX-License-Identifier: MIT
 */

#include "ring_reader.h"

#include <random>
#include <utility>

#include "soak_plan.h"

namespace fnirsi::tools {

namespace br = bitoscope::bulk_ring;

namespace {

uint64_t randomCaptureId() {
  std::random_device entropy;
  while (true) {
    const uint64_t id = (static_cast<uint64_t>(entropy()) << 32) ^ entropy();
    if (id != 0) return id;
  }
}

}

std::expected<std::unique_ptr<RingReader>, std::string> RingReader::start(uint32_t slot_count,
                                                                         uint64_t slot_payload_bytes,
                                                                         ChunkFn on_chunk) {
  auto ring = br::BulkRing::create(br::makeRingName(), slot_count, slot_payload_bytes, randomCaptureId());
  if (!ring) return std::unexpected("cannot create bulk ring: " + ring.error());
  std::unique_ptr<RingReader> reader(new RingReader(std::move(*ring), std::move(on_chunk)));
  reader->thread_ = std::thread([r = reader.get()] { r->run(); });
  return reader;
}

RingReader::RingReader(br::BulkRing ring, ChunkFn on_chunk)
    : ring_(std::move(ring)), on_chunk_(std::move(on_chunk)) {}

RingReader::~RingReader() { stop(); }

void RingReader::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

std::string RingReader::endReason() const {
  std::lock_guard lock(mutex_);
  return end_reason_;
}

RingReader::End RingReader::end() const {
  std::lock_guard lock(mutex_);
  return end_;
}

std::optional<br::FailedRecord> RingReader::failedRecord() const {
  std::lock_guard lock(mutex_);
  return failed_record_;
}

void RingReader::run() {
  const auto finish = [this](std::string reason, End end, std::optional<br::FailedRecord> record = std::nullopt) {
    std::lock_guard lock(mutex_);
    end_reason_ = std::move(reason);
    end_ = end;
    failed_record_ = std::move(record);
    failed_.store(end == End::Failed || end == End::Invalid);
  };
  while (!stop_.load()) {
    ring_.heartbeat();
    br::Poll poll = ring_.wait(ring_.nowMs() + 10);
    switch (poll.status) {
      case br::PollStatus::Empty:
        break;
      case br::PollStatus::Chunk: {
        const uint64_t in_use = ring_.produced() - ring_.consumed();
        if (in_use > peak_.load()) peak_.store(in_use);
        if (on_chunk_) on_chunk_(poll.chunk.header, poll.chunk.payload);
        (void)ring_.release();
        break;
      }
      case br::PollStatus::Ended:
        finish("driver ended stream", End::Ended);
        return;
      case br::PollStatus::Failed: {
        const br::FailedRecord record = ring_.failedRecord();
        finish(ringFailureText(record.at_sample, record.lost_samples, record.reason), End::Failed, record);
        return;
      }
      case br::PollStatus::Invalid:
        finish("bulk ring broke its protocol: " + poll.error, End::Invalid);
        return;
    }
  }
}

}

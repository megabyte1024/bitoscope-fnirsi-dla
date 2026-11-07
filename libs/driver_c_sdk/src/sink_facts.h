/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <span>

#include "bitoscope_driver.h"
#include "device_bytes.h"
#include "ipc_properties.h"

namespace bitoscope::plugin_c_sdk {

enum class Room { deviceBytes, capturedBytes };

struct SinkStreamFacts {
  uint64_t rateMilliHz = 0;
  bool buffered = false;
  bool starting = false;
  uint32_t silenceLimitMs = 0;
  std::span<const ipc::props::PropertyValue> properties;
  uint64_t unitBytes = 0;
  uint64_t unitSamples = 0;
  uint64_t chunkSamples = 0;
  const DeviceByteCompactor* compactor = nullptr;
};

SinkStreamFacts sinkStreamFacts(const bitoscope_sink& sink);

class SilenceWatchedByStream {
 public:
  explicit SilenceWatchedByStream(bitoscope_sink& sink);
  ~SilenceWatchedByStream();
  SilenceWatchedByStream(const SilenceWatchedByStream&) = delete;
  SilenceWatchedByStream& operator=(const SilenceWatchedByStream&) = delete;

 private:
  bitoscope_sink& sink_;
};

void* beginSamples(bitoscope_sink* sink, uint64_t max_samples, bool wait, Room room);

int commitSamples(bitoscope_sink* sink, uint64_t num_samples, Room room);

}

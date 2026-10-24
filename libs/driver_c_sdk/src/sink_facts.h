/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

#include "bitoscope_driver.h"
#include "device_bytes.h"

namespace bitoscope::plugin_c_sdk {

enum class Room { deviceBytes, capturedBytes };

struct SinkStreamFacts {
  uint64_t rateMilliHz = 0;
  bool buffered = false;
  bool starting = false;
  uint64_t unitBytes = 0;
  uint64_t unitSamples = 0;
  uint64_t chunkSamples = 0;
  const DeviceByteCompactor* compactor = nullptr;
};

SinkStreamFacts sinkStreamFacts(const bitoscope_sink& sink);

void* beginSamples(bitoscope_sink* sink, uint64_t max_samples, bool wait, Room room);

int commitSamples(bitoscope_sink* sink, uint64_t num_samples, Room room);

}

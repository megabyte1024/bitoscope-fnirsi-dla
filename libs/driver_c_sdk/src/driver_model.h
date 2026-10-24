/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bitoscope_driver.h"

namespace bitoscope::plugin_c_sdk {

inline constexpr const char* kLaFacetName = "la.acquisition";
inline constexpr uint32_t kLaFacetMajor = 1;
inline constexpr uint32_t kLaFacetMinor = 0;

struct LogicAnalyzerRegistration {
  uint32_t numChannels = 0;
  std::vector<uint64_t> sampleRatesMilliHz;
  bitoscope_capture_fn capture = nullptr;
  bitoscope_device_bytes deviceBytes{};
  void* session = nullptr;
  bitoscope_prepare_fn prepare = nullptr;
  std::vector<uint64_t> rateCeilingsMilliHz;
  std::string rateCeilingReason;
  bitoscope_apply_properties_fn applyProperties = nullptr;
};

struct SignalGeneratorRegistration {
  bitoscope_generator_output_fn start = nullptr;
  bitoscope_generator_stop_fn stop = nullptr;
  bitoscope_generator_output_fn configure = nullptr;
  void* session = nullptr;
  bitoscope_apply_properties_fn applyProperties = nullptr;
};

struct GenericRegistration {
  bitoscope_command_fn command = nullptr;
  void* session = nullptr;
  bitoscope_apply_properties_fn applyProperties = nullptr;
};

}

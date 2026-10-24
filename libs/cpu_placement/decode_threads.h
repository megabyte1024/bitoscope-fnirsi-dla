/*
 * SPDX-License-Identifier: MIT
 */

#pragma once


#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitoscope::cpu {

struct ProcessorCore {
  std::vector<uint32_t> hardware_threads;
  bool operator==(const ProcessorCore&) const = default;
};

struct ProcessorLayout {
  std::vector<ProcessorCore> cores;
  bool operator==(const ProcessorLayout&) const = default;
};

struct DecodeThreads {
  std::vector<uint32_t> hardware_threads;
  std::string note;
};

DecodeThreads decodeThreads(const ProcessorLayout& layout, std::optional<uint32_t> count = std::nullopt);

std::expected<ProcessorLayout, std::string> readProcessorLayout();

std::expected<std::vector<uint32_t>, std::string> parseCpuList(std::string_view text);

}

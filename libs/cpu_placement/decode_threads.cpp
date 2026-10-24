/*
 * SPDX-License-Identifier: MIT
 */

#include "decode_threads.h"

#include <algorithm>
#include <charconv>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#endif

namespace bitoscope::cpu {

DecodeThreads decodeThreads(const ProcessorLayout& layout, std::optional<uint32_t> count) {
  DecodeThreads out;
  uint64_t available = 0;
  for (const ProcessorCore& core : layout.cores) available += core.hardware_threads.size();
  if (available == 0) {
    out.note = "processor description has no cores: decoding is not placed";
    return out;
  }
  const uint64_t asked = count ? uint64_t{*count} : uint64_t{layout.cores.size()};
  const uint64_t used = (std::clamp)(asked, uint64_t{1}, available);
  if (asked == 0) {
    out.note = "decode thread count 0 was raised to 1";
  } else if (asked > available) {
    out.note = "decode thread count " + std::to_string(asked) + " was lowered to " + std::to_string(available) +
               ": processor has " + std::to_string(available) + " hardware threads";
  }
  for (size_t round = 0; out.hardware_threads.size() < used; ++round) {
    for (const ProcessorCore& core : layout.cores) {
      if (round < core.hardware_threads.size() && out.hardware_threads.size() < used) {
        out.hardware_threads.push_back(core.hardware_threads[round]);
      }
    }
  }
  std::sort(out.hardware_threads.begin(), out.hardware_threads.end());
  return out;
}

std::expected<std::vector<uint32_t>, std::string> parseCpuList(std::string_view text) {
  constexpr uint32_t kHighestCpu = 65535;
  const std::string shown(text.substr(0, text.find_last_not_of(" \t\r\n") + 1));
  const auto refuse = [&shown](std::string_view why) {
    return std::unexpected("CPU list '" + shown + "' does not read: " + std::string(why));
  };
  const auto number = [](std::string_view digits) -> std::optional<uint32_t> {
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || error != std::errc() || end != digits.data() + digits.size() || value > kHighestCpu) {
      return std::nullopt;
    }
    return value;
  };
  if (shown.empty()) return refuse("it is empty");
  std::vector<uint32_t> threads;
  std::string_view rest = shown;
  for (;;) {
    const size_t comma = rest.find(',');
    const std::string_view part = rest.substr(0, comma);
    const size_t dash = part.find('-');
    const auto first = number(part.substr(0, dash));
    const auto last = dash == std::string_view::npos ? first : number(part.substr(dash + 1));
    if (!first || !last) return refuse("'" + std::string(part) + "' is not CPU number or range of them");
    if (*last < *first) return refuse("range '" + std::string(part) + "' runs backwards");
    for (uint32_t thread = *first; thread <= *last; ++thread) threads.push_back(thread);
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  std::sort(threads.begin(), threads.end());
  threads.erase(std::unique(threads.begin(), threads.end()), threads.end());
  return threads;
}

#if defined(_WIN32) || defined(__linux__)

namespace {

const char* const kTooManyThreads = "placement on hardware threads is not done on machine with more than 64 hardware threads";

void sortCores(ProcessorLayout& layout) {
  std::sort(layout.cores.begin(), layout.cores.end(), [](const ProcessorCore& a, const ProcessorCore& b) {
    return a.hardware_threads.front() < b.hardware_threads.front();
  });
}

}

#endif

#if defined(_WIN32)

std::expected<ProcessorLayout, std::string> readProcessorLayout() {
  const auto failed = [](DWORD error) {
    return std::unexpected("processor's cores could not be read (error " + std::to_string(error) + ")");
  };
  DWORD bytes = 0;
  if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return failed(GetLastError());
  }
  std::vector<uint8_t> buffer(bytes);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes)) {
    return failed(GetLastError());
  }
  ProcessorLayout layout;
  for (DWORD at = 0; at < bytes;) {
    const auto* record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
    if (record->Size == 0) break;
    if (record->Relationship == RelationProcessorCore) {
      ProcessorCore core;
      for (WORD g = 0; g < record->Processor.GroupCount; ++g) {
        const GROUP_AFFINITY& group = record->Processor.GroupMask[g];
        if (group.Group != 0) return std::unexpected(std::string(kTooManyThreads));
        for (uint32_t bit = 0; bit < 64; ++bit) {
          if (((uint64_t{group.Mask} >> bit) & 1u) != 0) core.hardware_threads.push_back(bit);
        }
      }
      if (!core.hardware_threads.empty()) layout.cores.push_back(std::move(core));
    }
    at += record->Size;
  }
  if (layout.cores.empty()) return std::unexpected(std::string("processor reported no cores"));
  sortCores(layout);
  return layout;
}

#elif defined(__linux__)

std::expected<ProcessorLayout, std::string> readProcessorLayout() {
  namespace fs = std::filesystem;
  const fs::path cpus = "/sys/devices/system/cpu";
  std::map<uint32_t, ProcessorCore> by_first_thread;
  std::error_code error;
  for (fs::directory_iterator it(cpus, error), end; !error && it != end; it.increment(error)) {
    const std::string name = it->path().filename().string();
    if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 ||
        !std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    std::ifstream file(it->path() / "topology" / "thread_siblings_list");
    std::string text;
    if (!file || !std::getline(file, text)) continue;
    auto siblings = parseCpuList(text);
    if (!siblings) return std::unexpected(name + "'s " + siblings.error());
    const uint32_t first = siblings->front();
    by_first_thread.try_emplace(first, ProcessorCore{std::move(*siblings)});
  }
  if (error) return std::unexpected(cpus.string() + " could not be read: " + error.message());
  if (by_first_thread.empty()) return std::unexpected("no core was found under " + cpus.string());
  ProcessorLayout layout;
  for (auto& [first, core] : by_first_thread) {
    if (core.hardware_threads.back() > 63) return std::unexpected(std::string(kTooManyThreads));
    layout.cores.push_back(std::move(core));
  }
  sortCores(layout);
  return layout;
}

#else

std::expected<ProcessorLayout, std::string> readProcessorLayout() {
  return std::unexpected(std::string("placement on hardware threads is not done on this system"));
}

#endif

}

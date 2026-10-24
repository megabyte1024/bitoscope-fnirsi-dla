/*
 * SPDX-License-Identifier: MIT
 */

#include "work_placement.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstring>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bitoscope::cpu {

std::expected<uint64_t, std::string> hardwareThreadMask(std::span<const uint32_t> hardware_threads) {
  uint64_t mask = 0;
  for (const uint32_t thread : hardware_threads) {
    if (thread > 63) {
      return std::unexpected("hardware thread " + std::to_string(thread) +
                             " cannot be named: placement names hardware threads 0 to 63");
    }
    mask |= uint64_t{1} << thread;
  }
  return mask;
}

std::string hardwareThreadList(std::span<const uint32_t> hardware_threads) {
  std::string text;
  for (const uint32_t thread : hardware_threads) text += (text.empty() ? "" : " ") + std::to_string(thread);
  return text;
}

#if defined(_WIN32)

std::expected<void, std::string> applyToThisThread(const WorkPlacement& placement) {
  const auto mask = hardwareThreadMask(placement.hardware_threads);
  if (!mask) return std::unexpected(mask.error());
  if (placement.below_normal_priority && !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) {
    return std::unexpected("thread priority could not be lowered: error " + std::to_string(GetLastError()));
  }
  if (*mask != 0 && SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(*mask)) == 0) {
    return std::unexpected("thread could not be placed on hardware threads " + hardwareThreadList(placement.hardware_threads) +
                           ": error " + std::to_string(GetLastError()));
  }
  return {};
}

#elif defined(__linux__)

std::expected<void, std::string> applyToThisThread(const WorkPlacement& placement) {
  const auto mask = hardwareThreadMask(placement.hardware_threads);
  if (!mask) return std::unexpected(mask.error());
  if (placement.below_normal_priority) {
    const id_t self = static_cast<id_t>(::syscall(SYS_gettid));
    errno = 0;
    const int nice_value = ::getpriority(PRIO_PROCESS, self);
    if (nice_value == -1 && errno != 0) {
      return std::unexpected(std::string("thread priority could not be read: ") + std::strerror(errno));
    }
    if (nice_value < kBelowNormalNice && ::setpriority(PRIO_PROCESS, self, kBelowNormalNice) != 0) {
      return std::unexpected(std::string("thread priority could not be lowered: ") + std::strerror(errno));
    }
  }
  if (*mask != 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const uint32_t thread : placement.hardware_threads) CPU_SET(thread, &set);
    if (::sched_setaffinity(0, sizeof set, &set) != 0) {
      return std::unexpected("thread could not be placed on hardware threads " + hardwareThreadList(placement.hardware_threads) + ": " +
                             std::strerror(errno));
    }
  }
  return {};
}

#else

std::expected<void, std::string> applyToThisThread(const WorkPlacement& placement) {
  if (placement.below_normal_priority) return std::unexpected(std::string("lower thread priority is not done on this system"));
  if (!placement.hardware_threads.empty()) {
    return std::unexpected(std::string("placement on hardware threads is not done on this system"));
  }
  return {};
}

#endif

}

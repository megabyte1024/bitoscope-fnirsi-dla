/*
 * SPDX-License-Identifier: MIT
 */

#include "identity_cache.h"

#include <algorithm>
#include <set>
#include <utility>

#include "driver_calls.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

IdentityCache::IdentityCache(const bitoscope_driver& driver, const bitoscope_connection& connection,
                             DeviceSource& source)
    : driver_(driver), connection_(connection), source_(source) {}

std::vector<ipc::DeviceInfo> IdentityCache::list(std::span<const SourceDevice> found, SteadyTime now) {
  std::vector<ipc::DeviceInfo> out;
  std::set<std::string> present;
  for (const SourceDevice& located : found) {
    present.insert(located.id);
    auto known = known_.find(located.id);
    if (known != known_.end() &&
        (known->second.absentSince || known->second.appearanceKey != located.appearanceKey)) {
      known_.erase(known);
      known = known_.end();
    }
    if (known == known_.end()) {
      known = known_.emplace(located.id, Known{}).first;
      known->second.appearanceKey = located.appearanceKey;
    }
    Known& device = known->second;
    if (device.notSupported) continue;

    const AvailabilityCheck checked = source_.check(located);
    if (checked.availability != Availability::free) {
      device.checkFoundUnavailable = true;
      device.listed = unavailableEntry(device, located, reasonOfCheck(checked.availability), checked.detail);
      out.push_back(device.listed);
      continue;
    }
    if (identifyDue(device, now)) identify(device, located, now);
    if (device.notSupported) continue;
    if (device.failure == Failure::none && device.identified) {
      device.listed = *device.identified;
    } else {
      ipc::UnavailableReason reason = ipc::UnavailableReason::other;
      switch (device.failure) {
        case Failure::busy:
          reason = ipc::UnavailableReason::busy;
          break;
        case Failure::noPermission:
          reason = ipc::UnavailableReason::noPermission;
          break;
        case Failure::firmwareLoading:
          reason = ipc::UnavailableReason::firmwareLoading;
          break;
        case Failure::none:
        case Failure::other:
          break;
      }
      device.listed = unavailableEntry(device, located, reason, device.failureDetail);
    }
    out.push_back(device.listed);
  }

  for (auto known = known_.begin(); known != known_.end();) {
    Known& device = known->second;
    if (present.contains(known->first)) {
      ++known;
      continue;
    }
    if (device.failure == Failure::firmwareLoading) {
      if (!device.absentSince) device.absentSince = now;
      if (now - *device.absentSince < kFirmwareGracePeriod) {
        out.push_back(device.listed);
        ++known;
        continue;
      }
    }
    known = known_.erase(known);
  }
  return out;
}

bool IdentityCache::identifyDue(const Known& known, SteadyTime now) const {
  switch (known.failure) {
    case Failure::noPermission:
    case Failure::firmwareLoading:
      return false;
    case Failure::other:
      return now >= known.otherRetryAt;
    case Failure::busy:
      return true;
    case Failure::none:
      break;
  }
  return !known.identified || known.checkFoundUnavailable;
}

void IdentityCache::identify(Known& known, const SourceDevice& found, SteadyTime now) {
  known.checkFoundUnavailable = false;
  const GivenDevice given(found, connection_);
  DeviceDescription description(found.id);
  Attempt attempt = callIdentify(driver_, given.get(), description.device());
  if (attempt.status == BITOSCOPE_OK) {
    ipc::DeviceInfo described = description.info(genericNameOf(driver_, found));
    described.identified = true;
    known.identified = std::move(described);
    known.failure = Failure::none;
    known.failureDetail.clear();
    known.otherRetryDelay = std::chrono::seconds{0};
    return;
  }
  known.failureDetail.clear();
  switch (attempt.status) {
    case BITOSCOPE_NOT_SUPPORTED:
      known.notSupported = true;
      logLine(ipc::LogLevel::info, "device \"" + found.id + "\" is not supported by this driver" +
                                       (attempt.text.empty() ? std::string() : ": " + attempt.text) +
                                       "; it is not listed");
      return;
    case BITOSCOPE_BUSY:
      known.failure = Failure::busy;
      return;
    case BITOSCOPE_NO_PERMISSION:
      known.failure = Failure::noPermission;
      return;
    case BITOSCOPE_FIRMWARE_LOADING:
      known.failure = Failure::firmwareLoading;
      return;
    default:
      break;
  }
  known.failure = Failure::other;
  known.failureDetail = attempt.text.empty()
                            ? "driver's identify function returned " + std::to_string(attempt.status)
                            : std::move(attempt.text);
  known.otherRetryDelay = known.otherRetryDelay == std::chrono::seconds{0}
                              ? kFirstOtherRetry
                              : (std::min)(known.otherRetryDelay * 2, std::chrono::seconds{kLongestOtherRetry});
  known.otherRetryAt = now + known.otherRetryDelay;
}

ipc::DeviceInfo IdentityCache::genericEntry(const SourceDevice& found) const {
  ipc::DeviceInfo info;
  info.id = found.id;
  info.name = genericNameOf(driver_, found);
  return info;
}

ipc::DeviceInfo IdentityCache::unavailableEntry(const Known& known, const SourceDevice& found,
                                                ipc::UnavailableReason reason, const std::string& detail) const {
  ipc::DeviceInfo info = known.identified ? *known.identified : genericEntry(found);
  info.unavailable = reason;
  info.unavailableDetail = detail;
  return info;
}

}

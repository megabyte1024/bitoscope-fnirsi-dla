/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "device_source.h"
#include "frame_output.h"
#include "ipc_byte_stream.h"
#include "ipc_messages.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

class DeviceChangeSource {
 public:
  virtual ~DeviceChangeSource() = default;
  virtual std::expected<void, std::string> start(std::vector<UsbId> ids, std::function<void()> changed,
                                                 std::function<void(std::string_view why)> failed) = 0;
  virtual void stop() = 0;
};

std::unique_ptr<DeviceChangeSource> makeSystemDeviceChangeSource();

using ChangeSourceFactory = std::function<std::unique_ptr<DeviceChangeSource>()>;

bool symbolicLinkMatches(std::wstring_view link, const std::vector<UsbId>& ids);

enum class UsbHotplugAction { added, removed };

std::optional<UsbHotplugAction> parseUsbUevent(std::string_view raw, const std::vector<UsbId>& ids);

enum class UeventRecvAction {
  parseMessage,
  readAgain,
  rescanDeviceList,
  stopReading,
};

UeventRecvAction classifyUeventRecv(std::int64_t bytes, int errorNumber);

std::vector<ipc::Envelope> entryChanges(const std::vector<ipc::DeviceInfo>& before,
                                        const std::vector<ipc::DeviceInfo>& after);

struct WatchTiming {
  std::chrono::milliseconds settleAfterLastNotice{250};
  std::chrono::milliseconds recheckAfterChangeList{1500};
};

inline constexpr std::chrono::seconds kListAgainMergeWindow{1};

class WatchClock {
 public:
  virtual ~WatchClock() = default;
  virtual std::chrono::steady_clock::time_point now() const = 0;
  virtual void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& woken,
                    std::optional<std::chrono::steady_clock::time_point> until) = 0;
};

WatchClock& steadyWatchClock();

using Relist = std::function<std::expected<std::vector<ipc::DeviceInfo>, std::string>()>;

struct WatchReport {
  std::string driverName;
  std::string pluginVersion;
};

int runWatch(ipc::ByteSource& in, FrameOutput& out, std::vector<ipc::DeviceInfo> initial, const Relist& relist,
             ChangeNotices& notices, WatchTiming timing, const WatchReport& report = {},
             WatchClock& clock = steadyWatchClock());

}

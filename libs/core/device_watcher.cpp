/*
 * SPDX-License-Identifier: MIT
 */

#include "device_watcher.h"

#include <algorithm>
#include <condition_variable>
#include <optional>
#include <utility>
#include <variant>

#include "ipc_frame.h"

namespace bitoscope::host {

namespace {

constexpr std::chrono::milliseconds kStopGrace{2000};

std::string exitText(PluginLink& link) {
  const auto code = link.waitForExit(std::chrono::milliseconds(2000));
  return code ? "exited (code " + std::to_string(*code) + ")" : "ended its output";
}

std::expected<ipc::DeviceList, std::string> readFirstList(PluginLink& link, ipc::FrameReader& reader) {
  const std::string& who = link.name();
  auto frame = reader.next();
  if (!frame) return std::unexpected(who + ": --watch output broke off: " + frame.error().message);
  if (!frame->has_value()) return std::unexpected(who + ": --watch wrote no device list; driver " + exitText(link));
  auto envelope = ipc::decodeEnvelope(**frame);
  if (!envelope) {
    return std::unexpected(who + ": protocol error: its --watch output does not decode (" + envelope.error().message + ")");
  }
  auto* list = std::get_if<ipc::DeviceList>(&envelope->body);
  if (list == nullptr) return std::unexpected(who + ": protocol error: --watch did not start with device list");
  if (!list->supportsWatch) return std::unexpected(who + ": driver cannot watch for devices (its device list says so)");
  return std::move(*list);
}

}

std::vector<std::string> watchArguments(const DriverConnection& connection) {
  return driverArguments({"--watch"}, connection);
}

std::vector<std::string> driverArguments(std::vector<std::string> modeArguments, const DriverConnection& connection) {
  std::vector<std::string> arguments = std::move(modeArguments);
  const auto add = [&arguments](std::string option, std::string value) {
    arguments.push_back(std::move(option));
    arguments.push_back(std::move(value));
  };
  if (!connection.networkAddress.empty()) add("--network-address", connection.networkAddress);
  if (connection.networkPort) add("--network-port", std::to_string(*connection.networkPort));
  if (!connection.serialPort.empty()) add("--serial-port", connection.serialPort);
  return arguments;
}

std::expected<std::unique_ptr<DeviceWatcher>, std::string> DeviceWatcher::start(std::unique_ptr<PluginLink> link,
                                                                                 DeviceWatcherEvents events,
                                                                                 std::chrono::milliseconds timeout) {
  std::mutex mutex;
  std::condition_variable cv;
  bool finished = false;
  bool timedOut = false;
  PluginLink& l = *link;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, timeout, [&] { return finished; })) {
      timedOut = true;
      lock.unlock();
      l.shutdown(std::chrono::milliseconds(0));
    }
  });
  ipc::FrameReader reader(link->fromPlugin());
  auto first = readFirstList(*link, reader);
  {
    std::lock_guard lock(mutex);
    finished = true;
  }
  cv.notify_all();
  watchdog.join();

  if (timedOut) {
    return std::unexpected(link->name() + ": --watch wrote no device list within " + std::to_string(timeout.count()) +
                           " ms; it was ended");
  }
  if (!first) {
    link->shutdown(kStopGrace);
    return std::unexpected(first.error());
  }
  if (events.onDeviceAppeared) {
    for (const ipc::DeviceInfo& device : first->devices) events.onDeviceAppeared(device);
  }
  std::unique_ptr<DeviceWatcher> watcher(new DeviceWatcher(std::move(link), std::move(events), std::move(first->devices)));
  watcher->reader_ = std::thread([w = watcher.get()] { w->readLoop(); });
  return watcher;
}

std::expected<std::unique_ptr<DeviceWatcher>, std::string> DeviceWatcher::start(const std::filesystem::path& executable,
                                                                                 std::string name,
                                                                                 StderrLineFn onStderrLine,
                                                                                 DeviceWatcherEvents events,
                                                                                 std::chrono::milliseconds timeout,
                                                                                 const DriverConnection& connection) {
  auto started = PluginProcess::start(
      PluginProcessOptions{executable, watchArguments(connection), std::move(name), std::move(onStderrLine)});
  if (!started) return std::unexpected(started.error());
  return start(std::unique_ptr<PluginLink>(std::move(*started)), std::move(events), timeout);
}

DeviceWatcher::DeviceWatcher(std::unique_ptr<PluginLink> link, DeviceWatcherEvents events,
                             std::vector<ipc::DeviceInfo> devices)
    : link_(std::move(link)), events_(std::move(events)), devices_(std::move(devices)) {}

DeviceWatcher::~DeviceWatcher() { stop(); }

void DeviceWatcher::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  link_->shutdown(kStopGrace);
  if (reader_.joinable()) reader_.join();
}

std::expected<void, std::string> DeviceWatcher::listAgain() {
  auto bytes = ipc::encodeEnvelope(ipc::Envelope{0, ipc::ListAgain{}});
  if (!bytes) return std::unexpected(link_->name() + ": list request could not be encoded: " + bytes.error());
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return std::unexpected(link_->name() + ": device watch stopped; list request not sent");
  }
  std::lock_guard writing(writeMutex_);
  ipc::FrameWriter writer(link_->toPlugin());
  if (auto wrote = writer.write(*bytes); !wrote) {
    return std::unexpected(link_->name() + ": list request could not be sent: " + wrote.error().message);
  }
  return {};
}

std::vector<ipc::DeviceInfo> DeviceWatcher::devices() const {
  std::lock_guard lock(mutex_);
  return devices_;
}

void DeviceWatcher::readLoop() {
  ipc::FrameReader reader(link_->fromPlugin());
  const std::string& who = link_->name();
  std::optional<WatchEnd> ended;
  bool toldByDriver = false;
  for (;;) {
    auto frame = reader.next();
    if (!frame) {
      const std::string cause = "protocol error: " + frame.error().message;
      ended = WatchEnd{who + ": " + cause, cause};
      break;
    }
    if (!frame->has_value()) {
      const std::string cause = "driver " + exitText(*link_);
      ended = WatchEnd{who + ": device watch ended: " + cause, cause};
      break;
    }
    auto envelope = ipc::decodeEnvelope(**frame);
    if (!envelope) {
      const std::string cause = "protocol error: message from --watch does not decode (" + envelope.error().message + ")";
      ended = WatchEnd{who + ": " + cause, cause};
      break;
    }
    if (const auto* listed = std::get_if<ipc::DeviceListed>(&envelope->body)) {
      bool changed = false;
      {
        std::lock_guard lock(mutex_);
        const auto known = std::ranges::find(devices_, listed->device.id, &ipc::DeviceInfo::id);
        changed = known != devices_.end();
        if (changed) {
          *known = listed->device;
        } else {
          devices_.push_back(listed->device);
        }
      }
      if (changed && events_.onDeviceChanged) {
        events_.onDeviceChanged(listed->device);
      } else if (events_.onDeviceAppeared) {
        events_.onDeviceAppeared(listed->device);
      }
    } else if (const auto* detached = std::get_if<ipc::DeviceDetached>(&envelope->body)) {
      {
        std::lock_guard lock(mutex_);
        std::erase_if(devices_, [&](const ipc::DeviceInfo& d) { return d.id == detached->id; });
      }
      if (events_.onDeviceLost) events_.onDeviceLost(detached->id, detached->reason);
    } else if (const auto* log = std::get_if<ipc::LogEvent>(&envelope->body)) {
      if (events_.onLog) events_.onLog(*log);
    } else if (const auto* failed = std::get_if<ipc::WatchFailed>(&envelope->body)) {
      const std::string cause = failed->reason.empty() ? std::string("driver gave no reason") : failed->reason;
      ended = WatchEnd{who + ": device watch stopped: " + cause, cause};
      toldByDriver = true;
      break;
    }
  }
  bool report = false;
  {
    std::lock_guard lock(mutex_);
    report = !stopping_;
  }
  if (!report) return;
  link_->shutdown(toldByDriver ? kStopGrace : std::chrono::milliseconds(0));
  if (events_.onEnded) events_.onEnded(*ended);
}

}

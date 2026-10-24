/*
 * SPDX-License-Identifier: MIT
 */

#include "pipe_plugin_host.h"

#include <condition_variable>
#include <thread>
#include <utility>
#include <variant>

#include "ipc_frame.h"

namespace bitoscope::host {

namespace {

std::string pathText(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::expected<ipc::DeviceList, std::string> readDeviceList(PluginProcess& process) {
  const std::string who = process.name();
  ipc::FrameReader reader(process.fromPlugin());
  auto frame = reader.next();
  if (!frame) {
    if (frame.error().kind == ipc::FrameErrorKind::tooLarge) {
      return std::unexpected(who + ": protocol error: its --list output is not driver protocol (" +
                             frame.error().message + ")");
    }
    return std::unexpected(who + ": --list output broke off: " + frame.error().message);
  }
  if (!frame->has_value()) return std::unexpected(who + ": --list wrote no device list");
  auto envelope = ipc::decodeEnvelope(**frame);
  if (!envelope) {
    return std::unexpected(who + ": protocol error: its --list output does not decode (" + envelope.error().message +
                           ")");
  }
  auto* list = std::get_if<ipc::DeviceList>(&envelope->body);
  if (list == nullptr) {
    return std::unexpected(who + ": protocol error: --list wrote something other than device list");
  }
  auto end = reader.next();
  if (!end || end->has_value()) {
    return std::unexpected(who + ": protocol error: --list wrote more than its one device list");
  }
  return std::move(*list);
}

}

std::expected<ipc::DeviceList, std::string> listDevices(const std::filesystem::path& executable,
                                                        const StderrLineFn& onStderrLine,
                                                        std::chrono::milliseconds timeout, std::string name,
                                                        const DriverConnection& connection) {
  auto started = PluginProcess::start(
      PluginProcessOptions{executable, driverArguments({"--list"}, connection), std::move(name), onStderrLine});
  if (!started) return std::unexpected(started.error());
  PluginProcess& process = **started;

  std::mutex mutex;
  std::condition_variable cv;
  bool finished = false;
  bool timedOut = false;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, timeout, [&] { return finished; })) {
      timedOut = true;
      lock.unlock();
      process.kill();
    }
  });
  auto result = readDeviceList(process);
  std::optional<int> code;
  if (result) code = process.waitForExit(timeout);
  {
    std::lock_guard lock(mutex);
    finished = true;
  }
  cv.notify_all();
  watchdog.join();

  if (timedOut) {
    return std::unexpected(process.name() + ": --list did not finish within " + std::to_string(timeout.count()) +
                           " ms; it was ended");
  }
  if (!result) return result;
  if (!code) return std::unexpected(process.name() + ": --list wrote its device list but did not exit");
  if (*code != 0) {
    return std::unexpected(process.name() + ": --list exited with code " + std::to_string(*code));
  }
  return result;
}

PipePluginHost::PipePluginHost(PipePluginHostOptions options) : options_(std::move(options)) {}

std::chrono::milliseconds PipePluginHost::startTimeout() const {
  return options_.startTimeout.count() > 0 ? options_.startTimeout : options_.requestTimeout;
}

PipePluginHost::~PipePluginHost() {
  std::map<std::string, std::shared_ptr<PluginConnection>, std::less<>> plugins;
  {
    std::lock_guard lock(mutex_);
    plugins.swap(plugins_);
  }
  for (auto& [id, connection] : plugins) connection->close();
}

std::vector<std::string> PipePluginHost::drivers() const {
  std::vector<std::string> out;
  for (const auto& path : listPluginExecutables(options_.pluginDirectory)) out.push_back(pathText(path.stem()));
  return out;
}

std::expected<std::filesystem::path, std::string> PipePluginHost::executableOf(std::string_view driver) const {
  for (const auto& path : listPluginExecutables(options_.pluginDirectory)) {
    if (pathText(path.stem()) == driver) return path;
  }
  return std::unexpected("no driver \"" + std::string(driver) + "\" in " + pathText(options_.pluginDirectory));
}

std::expected<ipc::DeviceList, std::string> PipePluginHost::listDevices(std::string_view driver,
                                                                          const DriverConnection& connection) const {
  auto executable = executableOf(driver);
  if (!executable) return std::unexpected(executable.error());
  return host::listDevices(*executable, options_.onStderrLine, startTimeout(), std::string(driver), connection);
}

std::expected<std::unique_ptr<DeviceWatcher>, std::string> PipePluginHost::watch(
    std::string_view driver, DeviceWatcherEvents events, const DriverConnection& connection) const {
  auto executable = executableOf(driver);
  if (!executable) return std::unexpected(executable.error());
  return DeviceWatcher::start(*executable, std::string(driver), options_.onStderrLine, std::move(events),
                              startTimeout(), connection);
}

std::expected<std::string, OpenRefusal> PipePluginHost::open(std::string_view driver, std::string_view deviceId,
                                                              HandshakeOffer offer,
                                                              const DriverConnection& connection) {
  std::lock_guard opening(openMutex_);
  const std::string id = std::string(driver) + "/" + std::string(deviceId);
  std::shared_ptr<PluginConnection> dead;
  {
    std::lock_guard lock(mutex_);
    if (auto it = plugins_.find(id); it != plugins_.end()) {
      if (!it->second->closedReason()) return id;
      dead = std::move(it->second);
      plugins_.erase(it);
    }
  }
  dead.reset();

  auto executable = executableOf(driver);
  if (!executable) return std::unexpected(OpenRefusal{executable.error()});
  auto process = PluginProcess::start(
      PluginProcessOptions{*executable, driverArguments({"--device", std::string(deviceId)}, connection), id, options_.onStderrLine});
  if (!process) return std::unexpected(OpenRefusal{process.error()});

  PluginEvents events;
  if (options_.onLog) events.onLog = [f = options_.onLog, id](const ipc::LogEvent& e) { f(id, e); };
  if (options_.onFacetEvent) events.onFacetEvent = [f = options_.onFacetEvent, id](const ipc::FacetEvent& e) { f(id, e); };
  if (options_.onClosed) events.onClosed = [f = options_.onClosed, id](std::string_view reason) { f(id, reason); };
  if (options_.onNotice) events.onNotice = [f = options_.onNotice, id](std::string_view text) { f(id, text); };
  if (options_.onDeviceLost) {
    events.onDeviceLost = [f = options_.onDeviceLost, id](std::string_view reason) { f(id, reason); };
  }
  if (options_.onOpenProgress) {
    events.onOpenProgress = [f = options_.onOpenProgress, id](const ipc::OpenProgress& p) { f(id, p); };
  }
  auto pluginConnection = std::make_shared<PluginConnection>(std::move(*process), std::move(events));

  auto reply = pluginConnection->handshake(offer, startTimeout());
  if (!reply) return std::unexpected(OpenRefusal{reply.error().message, reply.error().unavailable});
  std::lock_guard lock(mutex_);
  plugins_[id] = std::move(pluginConnection);
  return id;
}

bool PipePluginHost::close(std::string_view pluginId) {
  // WHY: One open() at a time, and never a close() inside one: an open() of the same device waits.
  std::lock_guard opening(openMutex_);
  std::shared_ptr<PluginConnection> connection;
  {
    std::lock_guard lock(mutex_);
    const auto it = plugins_.find(pluginId);
    if (it == plugins_.end()) return false;
    connection = std::move(it->second);
    plugins_.erase(it);
  }
  const bool wasOpen = !connection->closedReason().has_value();
  connection->close();  // WHY: Outside mutex_: close() waits for the driver's reader thread.
  return wasOpen;
}

std::shared_ptr<PluginConnection> PipePluginHost::connection(std::string_view pluginId) const {
  std::lock_guard lock(mutex_);
  auto it = plugins_.find(pluginId);
  return it == plugins_.end() ? nullptr : it->second;
}

std::expected<std::string, std::string> PipePluginHost::sendCommand(std::string_view pluginId, std::string_view facet,
                                                                    std::string_view payload) {
  const auto plugin = connection(pluginId);
  if (plugin == nullptr) {
    return std::unexpected(std::string(kTransportErrorPrefix) + "no driver \"" + std::string(pluginId) +
                           "\" was opened by this host");
  }
  const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  auto reply = plugin->command(facet, bytes, options_.requestTimeout);
  if (!reply) {
    const RequestError& error = reply.error();
    if (error.kind == RequestFailure::Rejected || error.kind == RequestFailure::Refused) {
      return std::unexpected(error.message);
    }
    return std::unexpected(std::string(kTransportErrorPrefix) + error.message);
  }
  return std::string(reply->begin(), reply->end());
}

std::expected<void, PropertiesRefusal> PipePluginHost::setProperties(std::string_view pluginId, std::string_view facet,
                                                                     std::span<const ipc::props::PropertyValue> values) {
  const auto plugin = connection(pluginId);
  if (plugin == nullptr) {
    return std::unexpected(PropertiesRefusal{
        std::string(kTransportErrorPrefix) + "no driver \"" + std::string(pluginId) + "\" was opened by this host", {}});
  }
  auto result = plugin->setProperties(facet, std::vector<ipc::props::PropertyValue>(values.begin(), values.end()),
                                      options_.requestTimeout);
  if (!result) {
    const RequestError& error = result.error();
    if (error.kind == RequestFailure::Rejected || error.kind == RequestFailure::Refused ||
        error.kind == RequestFailure::NoProperties) {
      return std::unexpected(PropertiesRefusal{error.message, error.property});
    }
    return std::unexpected(PropertiesRefusal{std::string(kTransportErrorPrefix) + error.message, {}});
  }
  return {};
}

}

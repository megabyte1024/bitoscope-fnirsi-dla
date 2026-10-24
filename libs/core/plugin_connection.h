/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: Callbacks run on the reader thread and must not wait for this connection's answer (self-wait).
// A command is never retried: a START that timed out may still have run.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ipc_messages.h"
#include "ipc_protocol_version.h"
#include "plugin_link.h"
#include "work_placement.h"

namespace bitoscope::host {

inline constexpr std::chrono::milliseconds kDefaultRequestTimeout{5000};
inline constexpr std::chrono::milliseconds kExitWait{2000};

enum class RequestFailure {
  NotReady,
  Closed,
  TimedOut,
  Rejected,
  Refused,
  SendFailed,
  Unexpected,
  NoProperties,
};

struct RequestError {
  RequestFailure kind = RequestFailure::Closed;
  std::string message;
  std::string property{};
  ipc::UnavailableReason unavailable = ipc::UnavailableReason::none;
};

struct HandshakeOffer {
  uint32_t minMajor = ipc::kProtocolMajor;
  uint32_t maxMajor = ipc::kProtocolMajor;
  uint32_t minor = ipc::kProtocolMinor;
  std::vector<ipc::FacetRequest> facets;
};

struct PluginEvents {
  std::function<void(const ipc::LogEvent&)> onLog;
  std::function<void(const ipc::FacetEvent&)> onFacetEvent;
  std::function<void(const ipc::DeviceListed&)> onDeviceListed;
  std::function<void(const ipc::DeviceDetached&)> onDeviceDetached;
  std::function<void(std::string_view reason)> onDeviceLost;
  std::function<void(std::string_view reason)> onClosed;
  std::function<void(std::string_view notice)> onNotice;
  std::function<void(const ipc::OpenProgress&)> onOpenProgress;
};

class PluginConnection {
 public:
  PluginConnection(std::unique_ptr<PluginLink> link, PluginEvents events, cpu::WorkPlacement reader = {});
  ~PluginConnection();
  PluginConnection(const PluginConnection&) = delete;
  PluginConnection& operator=(const PluginConnection&) = delete;

  std::expected<ipc::HandshakeReply, RequestError> handshake(const HandshakeOffer& offer,
                                                             std::chrono::milliseconds timeout = kDefaultRequestTimeout);

  std::expected<ipc::Bytes, RequestError> command(std::string_view facet, std::span<const uint8_t> payload,
                                                  std::chrono::milliseconds timeout = kDefaultRequestTimeout);

  std::expected<void, RequestError> setProperties(std::string_view facet, const std::vector<ipc::props::PropertyValue>& values,
                                                  std::chrono::milliseconds timeout = kDefaultRequestTimeout);

  void close(std::chrono::milliseconds grace = kShutdownGrace);

  const std::string& name() const;
  std::optional<ipc::DeviceInfo> device() const;
  std::string pluginVersion() const;
  std::optional<std::string> closedReason() const;
  std::optional<std::string> deviceLostReason() const;
  bool waitClosed(std::chrono::milliseconds timeout) const;

 private:
  struct Pending {
    bool done = false;
    std::expected<ipc::Envelope, RequestError> result = std::unexpected(RequestError{});
  };

  enum class Wait { fixed, restartedByOpenProgress };

  std::expected<ipc::Envelope, RequestError> request(ipc::Body body, std::string_view what,
                                                     std::chrono::milliseconds timeout, Wait wait = Wait::fixed);
  void readLoop();
  void dispatch(ipc::Envelope envelope);
  bool complete(uint64_t id, std::expected<ipc::Envelope, RequestError> result);
  void closeWith(std::string reason);
  std::string endedReason(std::string_view detail);
  void notice(std::string text);
  std::expected<void, std::string> send(const ipc::Envelope& envelope);

  std::unique_ptr<PluginLink> link_;
  PluginEvents events_;
  cpu::WorkPlacement reader_placement_;

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::map<uint64_t, Pending*> pending_;
  uint64_t nextId_ = 1;
  std::optional<std::string> closed_;
  std::optional<ipc::DeviceInfo> device_;
  std::string pluginVersion_;
  std::optional<std::string> deviceLost_;
  bool closing_ = false;
  bool handshaking_ = false;
  uint64_t openProgressReports_ = 0;

  std::mutex writeMutex_;
  std::thread reader_;
};

}

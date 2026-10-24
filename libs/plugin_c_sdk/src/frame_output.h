/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <expected>
#include <mutex>
#include <string>

#include "ipc_byte_stream.h"
#include "ipc_frame.h"
#include "ipc_messages.h"

namespace bitoscope::plugin_c_sdk {

struct SendError {
  bool broken = false;
  std::string message;
};

class FrameOutput {
 public:
  explicit FrameOutput(ipc::ByteSink& sink);

  std::expected<void, SendError> send(const ipc::Envelope& envelope);

  void setConnected(bool connected) { connected_.store(connected); }
  bool connected() const { return connected_.load(); }

  void setDeviceId(std::string id) { deviceId_ = std::move(id); }
  const std::string& deviceId() const { return deviceId_; }

 private:
  std::mutex mutex_;
  ipc::FrameWriter writer_;
  bool broken_ = false;
  std::atomic<bool> connected_{false};
  std::string deviceId_;
};

// WHY: Only one ActiveOutput may hold the slot: a second loop in one process once stole it and hung a test.
class ActiveOutput {
 public:
  explicit ActiveOutput(FrameOutput& output);
  ~ActiveOutput();
  ActiveOutput(const ActiveOutput&) = delete;
  ActiveOutput& operator=(const ActiveOutput&) = delete;

  bool held() const { return held_; }

 private:
  bool held_ = false;
};

void logLine(ipc::LogLevel level, const std::string& message);

std::expected<void, std::string> sendFacetEvent(std::string facet, ipc::Bytes payload);

std::expected<void, std::string> sendDeviceLost(std::string reason);

}

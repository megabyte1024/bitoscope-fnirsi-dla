/*
 * SPDX-License-Identifier: MIT
 */

#include "frame_output.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace bitoscope::plugin_c_sdk {

namespace {

// WHY: Held for a whole send, so an ending loop cannot free the output under a sender.
std::mutex gActiveMutex;
FrameOutput* gActive = nullptr;

const char* levelName(ipc::LogLevel level) {
  switch (level) {
    case ipc::LogLevel::debug:
      return "debug";
    case ipc::LogLevel::info:
      return "info";
    case ipc::LogLevel::warning:
      return "warning";
    case ipc::LogLevel::error:
      return "error";
    case ipc::LogLevel::unspecified:
      break;
  }
  return "log";
}

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}

FrameOutput::FrameOutput(ipc::ByteSink& sink) : writer_(sink) {}

std::expected<void, SendError> FrameOutput::send(const ipc::Envelope& envelope) {
  auto bytes = ipc::encodeEnvelope(envelope);
  if (!bytes) return std::unexpected(SendError{false, "message could not be encoded: " + bytes.error()});
  std::lock_guard lock(mutex_);
  if (broken_) return std::unexpected(SendError{true, "host's stream failed earlier"});
  auto wrote = writer_.write(*bytes);
  if (!wrote) {
    const bool broken = wrote.error().kind == ipc::FrameErrorKind::streamFailed;
    broken_ = broken_ || broken;
    return std::unexpected(SendError{broken, wrote.error().message});
  }
  return {};
}

ActiveOutput::ActiveOutput(FrameOutput& output) {
  std::lock_guard lock(gActiveMutex);
  if (gActive == nullptr) {
    gActive = &output;
    held_ = true;
  }
}

ActiveOutput::~ActiveOutput() {
  if (!held_) return;
  std::lock_guard lock(gActiveMutex);
  gActive = nullptr;
}

void logLine(ipc::LogLevel level, const std::string& message) {
  {
    std::lock_guard lock(gActiveMutex);
    if (gActive != nullptr && gActive->connected()) {
      if (gActive->send(ipc::Envelope{0, ipc::LogEvent{level, message, nowNs(), {}}})) return;
    }
  }
  std::fprintf(stderr, "[%s] %s\n", levelName(level), message.c_str());
  std::fflush(stderr);
}

std::expected<void, std::string> sendFacetEvent(std::string facet, ipc::Bytes payload) {
  std::lock_guard lock(gActiveMutex);
  if (gActive == nullptr || !gActive->connected()) {
    return std::unexpected(std::string("no host is connected (event can be sent once handshake is done)"));
  }
  auto sent = gActive->send(ipc::Envelope{0, ipc::FacetEvent{std::move(facet), std::move(payload)}});
  if (!sent) return std::unexpected(sent.error().message);
  return {};
}

std::expected<void, std::string> sendDeviceLost(std::string reason) {
  std::lock_guard lock(gActiveMutex);
  if (gActive == nullptr || !gActive->connected()) {
    return std::unexpected(std::string("no host is connected (host can be told once handshake is done)"));
  }
  auto sent = gActive->send(ipc::Envelope{0, ipc::DeviceDetached{gActive->deviceId(), std::move(reason)}});
  if (!sent) return std::unexpected(sent.error().message);
  return {};
}

}

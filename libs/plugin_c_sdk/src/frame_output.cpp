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

constexpr std::string_view kNotConnectedForEvent = "no host is connected (event can be sent once handshake is done)";

template <class Send>
std::expected<void, std::string> sendOnActive(std::string_view notConnected, const Send& send) {
  std::lock_guard lock(gActiveMutex);
  if (gActive == nullptr || !gActive->connected()) return std::unexpected(std::string(notConnected));
  auto sent = send(*gActive);
  if (!sent) return std::unexpected(sent.error().message);
  return {};
}

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}

FrameOutput::FrameOutput(ipc::ByteSink& sink) : writer_(sink) {}

std::expected<void, SendError> FrameOutput::send(const ipc::Envelope& envelope) {
  return write(ipc::encodeFrame(envelope));
}

std::expected<void, SendError> FrameOutput::sendFacetEvent(std::string_view facet,
                                                           const ipc::detail::MessageBuildFn& payload) {
  return write(ipc::encodeFacetEventFrame(facet, payload));
}

std::expected<void, SendError> FrameOutput::write(const std::expected<ipc::Frame, std::string>& frame) {
  if (!frame) return std::unexpected(SendError{false, "message could not be encoded: " + frame.error()});
  std::lock_guard lock(mutex_);
  if (broken_) return std::unexpected(SendError{true, "host's stream failed earlier"});
  auto wrote = writer_.write(*frame);
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
  return sendOnActive(kNotConnectedForEvent, [&](FrameOutput& output) {
    return output.send(ipc::Envelope{0, ipc::FacetEvent{std::move(facet), std::move(payload)}});
  });
}

std::expected<void, std::string> sendFacetEvent(std::string_view facet, const ipc::detail::MessageBuildFn& payload) {
  return sendOnActive(kNotConnectedForEvent,
                      [&](FrameOutput& output) { return output.sendFacetEvent(facet, payload); });
}

std::expected<void, std::string> sendDeviceLost(std::string reason) {
  return sendOnActive("no host is connected (host can be told once handshake is done)", [&](FrameOutput& output) {
    return output.send(ipc::Envelope{0, ipc::DeviceDetached{output.deviceId(), std::move(reason)}});
  });
}

}

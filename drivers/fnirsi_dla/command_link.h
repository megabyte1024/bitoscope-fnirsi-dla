/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fnirsi_protocol.h"

namespace fnirsi {

class BulkTransport {
 public:
  virtual ~BulkTransport() = default;

  virtual std::expected<size_t, std::string> bulkTransfer(uint8_t endpoint, std::span<uint8_t> data,
                                                          unsigned timeout_ms) = 0;
};

struct Farewell {
  FramedCommand cmd;
  std::vector<uint8_t> payload;
};

class CommandLink {
 public:
  explicit CommandLink(BulkTransport& transport) : transport_(transport) {}

  CommandLink(const CommandLink&) = delete;
  CommandLink& operator=(const CommandLink&) = delete;

  [[nodiscard]] std::expected<void, std::string> sendFramed(FramedCommand cmd,
                                                            std::span<const uint8_t> payload) {
    if (auto refused = refusal()) {
      return std::unexpected(std::move(*refused));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto refused = refusal()) {
      return std::unexpected(std::move(*refused));
    }
    return writeFramedLocked(cmd, payload);
  }

  [[nodiscard]] std::expected<std::vector<uint8_t>, std::string> sendDirect(
      DirectCommand cmd, std::span<const uint8_t> payload, bool read_reply) {
    if (auto refused = refusal()) {
      return std::unexpected(std::move(*refused));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto refused = refusal()) {
      return std::unexpected(std::move(*refused));
    }
    auto buf = buildDirectCommand(cmd, payload);
    if (auto written = transport_.bulkTransfer(kEndpointCommandOut, buf, 1000); !written) {
      return std::unexpected("command write failed: " + written.error());
    }
    if (!read_reply) {
      return std::vector<uint8_t>{};
    }
    std::vector<uint8_t> reply(kDirectCommandSize, 0);
    const auto read = transport_.bulkTransfer(kEndpointCommandIn, reply, 1000);
    if (!read) {
      return std::unexpected("command reply read failed: " + read.error());
    }
    reply.resize(*read);
    return reply;
  }

  // WHY: shutdown() holds the command lock across teardown so the USB device is never closed under a transfer.
  void shutdown(std::span<const Farewell> farewell, const std::function<void()>& teardown) {
    // WHY: Publish Closing BEFORE waiting for the lock: a plain mutex could otherwise starve shutdown() behind a
    // flood of senders.
    State expected = State::Open;
    state_.compare_exchange_strong(expected, State::Closing);

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load() == State::Closed) {
      return;
    }
    for (const Farewell& f : farewell) {
      (void)writeFramedLocked(f.cmd, f.payload);
    }
    state_.store(State::Closed);
    teardown();
  }

  bool isOpen() const { return state_.load() == State::Open; }

  void reopen() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.store(State::Open);
  }

 private:
  static constexpr const char* kClosedMessage = "command link is closed (device was shut down)";
  static constexpr const char* kClosingMessage =
      "command link is shutting down (shutdown is pending)";

  std::optional<std::string> refusal() const {
    switch (state_.load()) {
      case State::Open:
        return std::nullopt;
      case State::Closing:
        return std::string(kClosingMessage);
      case State::Closed:
        return std::string(kClosedMessage);
    }
    return std::nullopt;
  }

  std::expected<void, std::string> writeFramedLocked(FramedCommand cmd,
                                                     std::span<const uint8_t> payload) {
    auto buf = buildFramedCommand(cmd, payload);
    if (auto written = transport_.bulkTransfer(kEndpointCommandOut, buf, 2000); !written) {
      return std::unexpected("framed command write failed: " + written.error());
    }
    return {};
  }

  BulkTransport& transport_;
  std::mutex mutex_;
  enum class State { Open, Closing, Closed };
  std::atomic<State> state_{State::Open};
};

}

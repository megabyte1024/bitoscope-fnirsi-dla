/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fnirsi_protocol.h"

namespace fnirsi {

struct TransferFailure {
  std::string text;
  bool timed_out = false;
  size_t transferred = 0;
};

class BulkTransport {
 public:
  virtual ~BulkTransport() = default;

  virtual std::expected<size_t, TransferFailure> bulkTransfer(uint8_t endpoint, std::span<uint8_t> data,
                                                              unsigned timeout_ms) = 0;
};

struct Farewell {
  FramedCommand cmd;
  std::vector<uint8_t> payload;
};

class CommandLink {
 public:
  static constexpr int kQueryWriteAttempts = 6;

  explicit CommandLink(BulkTransport& transport, std::function<void(std::string_view)> note = {})
      : transport_(transport), note_(std::move(note)) {}

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
    const int attempts = changesNoState(cmd) ? kQueryWriteAttempts : 1;
    for (int attempt = 1;; ++attempt) {
      const auto written = transport_.bulkTransfer(kEndpointCommandOut, buf, 1000);
      if (written) {
        break;
      }
      const TransferFailure& failure = written.error();
      if (attempts == 1 || !failure.timed_out || failure.transferred != 0) {
        return std::unexpected("command write failed: " + failure.text);
      }
      if (attempt >= attempts) {
        return std::unexpected(
            std::format("command write failed: {} ({} writes, none taken)", failure.text, attempts));
      }
      if (note_) {
        note_(std::format("command 0a {:02x} write not taken ({}), sending it again: write {} of {}",
                          static_cast<unsigned>(cmd), failure.text, attempt + 1, attempts));
      }
    }
    if (!read_reply) {
      return std::vector<uint8_t>{};
    }
    std::vector<uint8_t> reply(kDirectCommandSize, 0);
    const auto read = transport_.bulkTransfer(kEndpointCommandIn, reply, 1000);
    if (!read) {
      return std::unexpected("command reply read failed: " + read.error().text);
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

  static constexpr bool changesNoState(DirectCommand cmd) {
    return cmd == DirectCommand::kFlush || cmd == DirectCommand::kGetMcuVersion ||
           cmd == DirectCommand::kGetDeviceData;
  }

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
      return std::unexpected("framed command write failed: " + written.error().text);
    }
    return {};
  }

  BulkTransport& transport_;
  std::function<void(std::string_view)> note_;
  std::mutex mutex_;
  enum class State { Open, Closing, Closed };
  std::atomic<State> state_{State::Open};
};

}

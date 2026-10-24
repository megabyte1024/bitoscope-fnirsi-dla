/*
 * SPDX-License-Identifier: MIT
 */

#include "plugin_connection.h"

#include "display_text.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "ipc_frame.h"

namespace bitoscope::host {

namespace {

std::string envelopeKind(const ipc::Body& body) {
  static constexpr const char* kNames[] = {"Handshake",      "HandshakeReply", "FacetCommand",  "FacetReply",
                                           "FacetEvent",     "LogEvent",       "DeviceList",    "DeviceListed",
                                           "DeviceDetached", "Refusal",        "SetProperties", "WatchFailed",
                                           "ListAgain",      "OpenProgress"};
  static_assert(std::size(kNames) == std::variant_size_v<ipc::Body>);
  return kNames[body.index()];
}

}

PluginConnection::PluginConnection(std::unique_ptr<PluginLink> link, PluginEvents events, cpu::WorkPlacement reader)
    : link_(std::move(link)), events_(std::move(events)), reader_placement_(std::move(reader)) {
  reader_ = std::thread([this] { readLoop(); });
}

PluginConnection::~PluginConnection() {
  close();
  if (reader_.joinable()) reader_.join();
}

const std::string& PluginConnection::name() const { return link_->name(); }

std::optional<ipc::DeviceInfo> PluginConnection::device() const {
  std::lock_guard lock(mutex_);
  return device_;
}

std::string PluginConnection::pluginVersion() const {
  std::lock_guard lock(mutex_);
  return pluginVersion_;
}

std::optional<std::string> PluginConnection::closedReason() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::optional<std::string> PluginConnection::deviceLostReason() const {
  std::lock_guard lock(mutex_);
  return deviceLost_;
}

bool PluginConnection::waitClosed(std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [this] { return closed_.has_value(); });
}

void PluginConnection::close(std::chrono::milliseconds grace) {
  {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    closing_ = true;
  }
  closeWith(name() + ": closed by host");
  link_->shutdown(grace);
}

std::expected<void, std::string> PluginConnection::send(const ipc::Envelope& envelope) {
  auto bytes = ipc::encodeEnvelope(envelope);
  if (!bytes) return std::unexpected("message could not be encoded: " + bytes.error());
  std::lock_guard lock(writeMutex_);
  ipc::FrameWriter writer(link_->toPlugin());
  auto wrote = writer.write(*bytes);
  if (!wrote) return std::unexpected(wrote.error().message);
  return {};
}

std::expected<ipc::Envelope, RequestError> PluginConnection::request(ipc::Body body, std::string_view what,
                                                                     std::chrono::milliseconds timeout, Wait wait) {
  Pending pending;
  uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    if (closed_) return std::unexpected(RequestError{RequestFailure::Closed, *closed_});
    id = nextId_++;
    pending_[id] = &pending;
  }
  auto forget = [&] {
    std::lock_guard lock(mutex_);
    pending_.erase(id);
  };

  if (auto sent = send(ipc::Envelope{id, std::move(body)}); !sent) {
    forget();
    std::unique_lock lock(mutex_);
    if (pending.done) return std::move(pending.result);
    // WHY: Write to dead plugin can fail before reader records its exit: wait for that reason, bounded.
    cv_.wait_for(lock, (std::min)(timeout, kExitWait), [this] { return closed_.has_value(); });
    if (closed_) return std::unexpected(RequestError{RequestFailure::Closed, *closed_});
    return std::unexpected(
        RequestError{RequestFailure::SendFailed, name() + ": " + std::string(what) + " could not be sent: " + sent.error()});
  }

  const bool restarts = wait == Wait::restartedByOpenProgress;
  std::unique_lock lock(mutex_);
  for (;;) {
    const uint64_t reports = openProgressReports_;
    if (cv_.wait_for(lock, timeout, [&] { return pending.done || (restarts && openProgressReports_ != reports); })) {
      if (pending.done) break;
      continue;
    }
    pending_.erase(id);
    return std::unexpected(RequestError{RequestFailure::TimedOut, name() + ": no answer to " + std::string(what) +
                                                                      " (request " + std::to_string(id) + ") within " +
                                                                      std::to_string(timeout.count()) + " ms"});
  }
  return std::move(pending.result);
}

std::expected<ipc::HandshakeReply, RequestError> PluginConnection::handshake(const HandshakeOffer& offer,
                                                                             std::chrono::milliseconds timeout) {
  {
    std::lock_guard lock(mutex_);
    handshaking_ = true;
  }
  auto answer = request(ipc::Handshake{offer.minMajor, offer.maxMajor, offer.minor, offer.facets}, "handshake",
                        timeout, Wait::restartedByOpenProgress);
  {
    std::lock_guard lock(mutex_);
    handshaking_ = false;
  }
  if (!answer) return std::unexpected(std::move(answer.error()));
  if (auto* refusal = std::get_if<ipc::Refusal>(&answer->body)) {
    return std::unexpected(
        RequestError{RequestFailure::Refused, name() + " refused handshake: " + refusal->message});
  }
  auto* reply = std::get_if<ipc::HandshakeReply>(&answer->body);
  if (reply == nullptr) {
    return std::unexpected(RequestError{RequestFailure::Unexpected, name() + " answered handshake with " +
                                                                        envelopeKind(answer->body)});
  }
  if (!reply->accepted) {
    return std::unexpected(RequestError{
        RequestFailure::Rejected,
        reply->message.empty() ? name() + " refused handshake without giving reason" : reply->message, {},
        reply->unavailable});
  }
  // WHY: Plugin abbreviations only fill gaps in this process's table: a plugin must not redefine settled
  // or user-defined text, nor change how another plugin's text reads.
  if (!reply->abbreviations.empty()) {
    std::vector<display_text::Abbreviation> usable;
    for (const ipc::Abbreviation& a : reply->abbreviations) {
      const display_text::Abbreviation entry{a.upper, a.shown, a.alsoWord};
      if (const auto problem = display_text::problemWith(entry)) {
        if (events_.onLog) {
          events_.onLog(ipc::LogEvent{ipc::LogLevel::warning,
                                      name() + " declares abbreviation that cannot be used: " + *problem, 0,
                                      "handshake"});
        }
        continue;
      }
      usable.push_back(entry);
    }
    display_text::addAbbreviations(usable);
  }
  return std::move(*reply);
}

std::expected<ipc::Bytes, RequestError> PluginConnection::command(std::string_view facet,
                                                                  std::span<const uint8_t> payload,
                                                                  std::chrono::milliseconds timeout) {
  {
    std::lock_guard lock(mutex_);
    if (closed_) return std::unexpected(RequestError{RequestFailure::Closed, *closed_});
    if (!device_) {
      return std::unexpected(
          RequestError{RequestFailure::NotReady, name() + ": no handshake yet: session starts with handshake"});
    }
  }
  const std::string what = std::string(facet) + " command";
  auto answer = request(ipc::FacetCommand{std::string(facet), ipc::Bytes(payload.begin(), payload.end())}, what, timeout);
  if (!answer) return std::unexpected(std::move(answer.error()));
  if (auto* refusal = std::get_if<ipc::Refusal>(&answer->body)) {
    return std::unexpected(RequestError{RequestFailure::Refused, refusal->message.empty()
                                                                     ? name() + " refused " + what + " without reason"
                                                                     : refusal->message});
  }
  auto* reply = std::get_if<ipc::FacetReply>(&answer->body);
  if (reply == nullptr) {
    return std::unexpected(
        RequestError{RequestFailure::Unexpected, name() + " answered " + what + " with " + envelopeKind(answer->body)});
  }
  if (!reply->ok) {
    return std::unexpected(RequestError{
        RequestFailure::Rejected, reply->message.empty() ? name() + " rejected " + what + " without reason" : reply->message});
  }
  return std::move(reply->payload);
}

std::expected<void, RequestError> PluginConnection::setProperties(std::string_view facet,
                                                                   const std::vector<ipc::props::PropertyValue>& values,
                                                                   std::chrono::milliseconds timeout) {
  {
    std::lock_guard lock(mutex_);
    if (closed_) return std::unexpected(RequestError{RequestFailure::Closed, *closed_});
    if (!device_) {
      return std::unexpected(
          RequestError{RequestFailure::NotReady, name() + ": no handshake yet: session starts with handshake"});
    }
    bool listed = false;
    for (const ipc::FacetInfo& info : device_->facets) {
      if (info.name == facet) {
        listed = !info.properties.empty();
        break;
      }
    }
    if (!listed) {
      return std::unexpected(RequestError{RequestFailure::NoProperties,
                                          name() + ": facet '" + std::string(facet) +
                                              "' lists no properties: nothing was sent"});
    }
  }
  const std::string what = "setProperties for " + std::string(facet);
  auto answer = request(ipc::props::SetProperties{std::string(facet), values}, what, timeout);
  if (!answer) return std::unexpected(std::move(answer.error()));
  if (auto* refusal = std::get_if<ipc::Refusal>(&answer->body)) {
    return std::unexpected(RequestError{RequestFailure::Refused, refusal->message.empty()
                                                                     ? name() + " refused " + what + " without reason"
                                                                     : refusal->message});
  }
  auto* reply = std::get_if<ipc::FacetReply>(&answer->body);
  if (reply == nullptr) {
    return std::unexpected(
        RequestError{RequestFailure::Unexpected, name() + " answered " + what + " with " + envelopeKind(answer->body)});
  }
  if (!reply->ok) {
    return std::unexpected(RequestError{
        RequestFailure::Rejected,
        reply->message.empty() ? name() + " rejected " + what + " without reason" : reply->message,
        reply->refusedProperty});
  }
  return {};
}

bool PluginConnection::complete(uint64_t id, std::expected<ipc::Envelope, RequestError> result) {
  {
    std::lock_guard lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) return false;
    it->second->result = std::move(result);
    it->second->done = true;
    pending_.erase(it);
  }
  cv_.notify_all();
  return true;
}

void PluginConnection::closeWith(std::string reason) {
  {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    closed_ = reason;
    for (auto& [id, pending] : pending_) {
      pending->result = std::unexpected(RequestError{RequestFailure::Closed, reason});
      pending->done = true;
    }
    pending_.clear();
  }
  cv_.notify_all();
  if (events_.onClosed) events_.onClosed(reason);
}

std::string PluginConnection::endedReason(std::string_view detail) {
  const std::optional<int> code = link_->waitForExit(kExitWait);
  if (!code) {
    link_->shutdown(std::chrono::milliseconds(0));
    return name() + ": plugin closed its output without exiting" + std::string(detail) + "; it was ended";
  }
  return name() + ": plugin exited (code " + std::to_string(*code) + ")" + std::string(detail);
}

void PluginConnection::notice(std::string text) {
  if (events_.onNotice) events_.onNotice(name() + ": " + text);
}

void PluginConnection::readLoop() {
  if (!reader_placement_.asksNothing()) {
    if (auto placed = cpu::applyToThisThread(reader_placement_); !placed) notice("reader thread: " + placed.error());
  }
  ipc::FrameReader reader(link_->fromPlugin());
  for (;;) {
    auto frame = reader.next();
    if (!frame) {
      const ipc::FrameError& error = frame.error();
      if (error.kind == ipc::FrameErrorKind::truncated) {
        closeWith(endedReason(" in middle of message"));
      } else if (error.kind == ipc::FrameErrorKind::streamFailed) {
        closeWith(endedReason(" (" + error.message + ")"));
      } else {
        closeWith(name() + ": protocol error: plugin's output is not plugin protocol (" + error.message + ")");
        link_->shutdown(kShutdownGrace);
      }
      return;
    }
    if (!frame->has_value()) {
      closeWith(endedReason(""));
      return;
    }
    auto envelope = ipc::decodeEnvelope(**frame);
    if (!envelope) {
      const ipc::DecodeError& error = envelope.error();
      if (error.kind == ipc::DecodeErrorKind::unknownMember) {
        const uint64_t id = error.requestId.value_or(0);
        const std::string what = "message this host does not know (envelope member " +
                                 std::to_string(error.member) + "; plugin is newer)";
        if (id != 0 && complete(id, std::unexpected(RequestError{RequestFailure::Unexpected,
                                                                 name() + " answered with " + what}))) {
          continue;
        }
        notice("skipped " + what);
        continue;
      }
      closeWith(name() + ": protocol error: plugin wrote message that does not decode (" + error.message + ")");
      link_->shutdown(kShutdownGrace);
      return;
    }
    dispatch(std::move(*envelope));
  }
}

void PluginConnection::dispatch(ipc::Envelope envelope) {
  const uint64_t id = envelope.requestId;
  std::visit(
      [&](auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, ipc::HandshakeReply> || std::is_same_v<T, ipc::FacetReply>) {
          if constexpr (std::is_same_v<T, ipc::HandshakeReply>) {
            // WHY: Record the device on the reader thread before handing the reply over: recorded by the waiter, it raced
            // a device-lost frame right behind the reply and the loss was missed.
            std::lock_guard lock(mutex_);
            if (body.accepted && pending_.count(id) != 0) {
              device_ = body.device;
              pluginVersion_ = body.pluginVersion;
            }
          }
          if (!complete(id, std::move(envelope))) {
            notice("answer to request " + std::to_string(id) + ", which nothing waits for (it timed out)");
          }
        } else if constexpr (std::is_same_v<T, ipc::Refusal>) {
          const std::string message = body.message;
          if (id == 0 || !complete(id, std::move(envelope))) {
            notice("plugin refused message" + (id == 0 ? std::string() : " (request " + std::to_string(id) + ")") +
                   ": " + message);
          }
        } else if constexpr (std::is_same_v<T, ipc::OpenProgress>) {
          bool handshaking = false;
          {
            std::lock_guard lock(mutex_);
            handshaking = handshaking_;
            if (handshaking) ++openProgressReports_;
          }
          cv_.notify_all();
          if (!handshaking) {
            notice("skipped open progress outside handshake: " + body.detail);
          } else if (events_.onOpenProgress) {
            events_.onOpenProgress(body);
          }
        } else if constexpr (std::is_same_v<T, ipc::LogEvent>) {
          if (events_.onLog) events_.onLog(body);
        } else if constexpr (std::is_same_v<T, ipc::FacetEvent>) {
          if (events_.onFacetEvent) events_.onFacetEvent(body);
        } else if constexpr (std::is_same_v<T, ipc::DeviceListed>) {
          if (events_.onDeviceListed) events_.onDeviceListed(body);
        } else if constexpr (std::is_same_v<T, ipc::DeviceDetached>) {
          bool lost = false;
          {
            std::lock_guard lock(mutex_);
            if (device_ && device_->id == body.id && !deviceLost_) {
              deviceLost_ = body.reason;
              lost = true;
            }
          }
          if (lost && events_.onDeviceLost) events_.onDeviceLost(body.reason);
          if (events_.onDeviceDetached) events_.onDeviceDetached(body);
        } else if constexpr (std::is_same_v<T, ipc::Handshake> || std::is_same_v<T, ipc::FacetCommand> ||
                             std::is_same_v<T, ipc::props::SetProperties>) {
          if (auto sent = send(ipc::Envelope{id, ipc::Refusal{"host does not take requests", 0}}); !sent) {
            notice("request from plugin could not be refused: " + sent.error());
          }
        } else {
          notice("skipped unexpected " + envelopeKind(envelope.body));
        }
      },
      envelope.body);
}

}

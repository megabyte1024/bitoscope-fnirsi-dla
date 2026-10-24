/*
 * SPDX-License-Identifier: MIT
 */

#include "device_watch.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include "ipc_frame.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

namespace {

using Clock = std::chrono::steady_clock;

std::optional<uint16_t> hexAfter(std::wstring_view link, std::wstring_view key) {
  auto upper = [](wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; };
  for (std::size_t at = 0; at + key.size() + 4 <= link.size(); ++at) {
    bool same = true;
    for (std::size_t i = 0; i < key.size() && same; ++i) same = upper(link[at + i]) == key[i];
    if (!same) continue;
    uint16_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const wchar_t c = upper(link[at + key.size() + i]);
      int digit = -1;
      if (c >= L'0' && c <= L'9') digit = c - L'0';
      if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
      if (digit < 0) return std::nullopt;
      value = static_cast<uint16_t>(value * 16 + digit);
    }
    return value;
  }
  return std::nullopt;
}

}

bool symbolicLinkMatches(std::wstring_view link, const std::vector<UsbId>& ids) {
  const auto vendor = hexAfter(link, L"VID_");
  const auto product = hexAfter(link, L"PID_");
  if (!vendor || !product) return false;
  return std::find(ids.begin(), ids.end(), UsbId{*vendor, *product}) != ids.end();
}

namespace {

std::optional<uint16_t> parseHexField(std::string_view field) {
  if (field.empty() || field.size() > 4) return std::nullopt;
  uint16_t value = 0;
  for (const char c : field) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return std::nullopt;
    value = static_cast<uint16_t>(value * 16 + digit);
  }
  return value;
}

}

UeventRecvAction classifyUeventRecv(std::int64_t bytes, int errorNumber) {
  if (bytes > 0) return UeventRecvAction::parseMessage;
  if (bytes == 0) return UeventRecvAction::readAgain;
  switch (errorNumber) {
    case ENOBUFS:
      return UeventRecvAction::rescanDeviceList;
    case EINTR:
    case EAGAIN:
    case ENOMEM:
      return UeventRecvAction::readAgain;
    default:
      return UeventRecvAction::stopReading;
  }
}

std::optional<UsbHotplugAction> parseUsbUevent(std::string_view raw, const std::vector<UsbId>& ids) {
  std::optional<std::string_view> action, subsystem, devtype, product;
  std::size_t start = 0;
  bool first = true;
  while (start <= raw.size()) {
    const std::size_t end = raw.find('\0', start);
    const std::string_view field = raw.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (first) {
      first = false;
    } else if (const std::size_t eq = field.find('='); eq != std::string_view::npos) {
      const std::string_view key = field.substr(0, eq);
      const std::string_view value = field.substr(eq + 1);
      if (key == "ACTION") action = value;
      else if (key == "SUBSYSTEM") subsystem = value;
      else if (key == "DEVTYPE") devtype = value;
      else if (key == "PRODUCT") product = value;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (!action || !subsystem || !devtype || !product) return std::nullopt;
  if (*subsystem != "usb" || *devtype != "usb_device") return std::nullopt;
  UsbHotplugAction hotplug;
  if (*action == "add") hotplug = UsbHotplugAction::added;
  else if (*action == "remove") hotplug = UsbHotplugAction::removed;
  else return std::nullopt;
  const std::size_t slash1 = product->find('/');
  if (slash1 == std::string_view::npos) return std::nullopt;
  const std::size_t slash2 = product->find('/', slash1 + 1);
  const std::string_view vendor_text = product->substr(0, slash1);
  const std::string_view product_text =
      product->substr(slash1 + 1, slash2 == std::string_view::npos ? std::string_view::npos : slash2 - slash1 - 1);
  const auto vendor = parseHexField(vendor_text);
  const auto prod = parseHexField(product_text);
  if (!vendor || !prod) return std::nullopt;
  if (std::find(ids.begin(), ids.end(), UsbId{*vendor, *prod}) == ids.end()) return std::nullopt;
  return hotplug;
}

namespace {

class SteadyWatchClock final : public WatchClock {
 public:
  std::chrono::steady_clock::time_point now() const override { return Clock::now(); }
  void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& woken,
            std::optional<std::chrono::steady_clock::time_point> until) override {
    if (until) {
      woken.wait_until(lock, *until);
    } else {
      woken.wait(lock);
    }
  }
};

}

WatchClock& steadyWatchClock() {
  static SteadyWatchClock clock;
  return clock;
}

std::vector<ipc::Envelope> entryChanges(const std::vector<ipc::DeviceInfo>& before,
                                        const std::vector<ipc::DeviceInfo>& after) {
  auto find = [](const std::vector<ipc::DeviceInfo>& list, const std::string& id) {
    return std::find_if(list.begin(), list.end(), [&](const ipc::DeviceInfo& d) { return d.id == id; });
  };
  std::vector<ipc::Envelope> out;
  for (const ipc::DeviceInfo& d : before) {
    if (find(after, d.id) == after.end()) out.push_back(ipc::Envelope{0, ipc::DeviceDetached{d.id, "removed"}});
  }
  for (const ipc::DeviceInfo& d : after) {
    const auto was = find(before, d.id);
    if (was == before.end() || *was != d) out.push_back(ipc::Envelope{0, ipc::DeviceListed{d}});
  }
  return out;
}

int runWatch(ipc::ByteSource& in, FrameOutput& out, std::vector<ipc::DeviceInfo> initial, const Relist& relist,
             ChangeNotices& notices, WatchTiming timing, const WatchReport& report, WatchClock& clock) {
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool changed = false;
    Clock::time_point lastChange;
    bool listAsked = false;
    bool hostGone = false;
    bool broken = false;
    std::optional<std::string> failure;
  };
  auto state = std::make_shared<State>();
  auto started = notices.start([state, &clock] {
    {
      std::lock_guard lock(state->mutex);
      state->changed = true;
      state->lastChange = clock.now();
    }
    state->cv.notify_all();
  }, [state](std::string_view why) {
    {
      std::lock_guard lock(state->mutex);
      if (state->failure) return;
      state->failure = std::string(why);
    }
    state->cv.notify_all();
  });
  if (!started) {
    logLine(ipc::LogLevel::error, "this driver cannot watch for devices: " + started.error());
    return kExitFailed;
  }

  if (auto sent = out.send(ipc::Envelope{0, ipc::DeviceList{initial, true, report.driverName, report.pluginVersion}}); !sent) {
    notices.stop();
    logLine(ipc::LogLevel::error, "device list could not be written: " + sent.error().message);
    return kExitFailed;
  }
  out.setConnected(true);

  std::thread watcher([state, &out, &relist, &clock, timing, current = std::move(initial),
                       lastListAt = clock.now()]() mutable {
    std::optional<Clock::time_point> recheckAt;
    std::unique_lock lock(state->mutex);
    for (;;) {
      if (state->hostGone) return;
      if (state->failure) {
        const ipc::WatchFailed failed{*state->failure};
        lock.unlock();
        const auto sent = out.send(ipc::Envelope{0, failed});
        lock.lock();
        if (!sent) state->broken = true;
        return;
      }
      const Clock::time_point now = clock.now();
      bool afterChange = false;
      if (state->changed && now >= state->lastChange + timing.settleAfterLastNotice) {
        state->changed = false;
        recheckAt = now + timing.recheckAfterChangeList;
        afterChange = true;
      } else if (!state->changed && recheckAt && now >= *recheckAt) {
        recheckAt.reset();
        afterChange = true;
      }
      bool asked = false;
      if (state->listAsked && now >= lastListAt + kListAgainMergeWindow) {
        state->listAsked = false;
        asked = true;
      }
      if (afterChange || asked) {
        lastListAt = now;
        lock.unlock();
        auto after = relist();
        bool broken = false;
        if (!after) {
          logLine(ipc::LogLevel::warning, "driver could not list its devices again: " + after.error());
        } else {
          const std::string why = afterChange && asked ? "after device change and at host's request"
                                  : afterChange        ? "after device change"
                                                       : "at host's request";
          logLine(ipc::LogLevel::debug, "listed devices again " + why + ": " + std::to_string(after->size()) +
                                            " device(s)");
          for (const ipc::Envelope& change : entryChanges(current, *after)) {
            if (auto sent = out.send(change); !sent) {
              broken = true;
              break;
            }
          }
          current = std::move(*after);
        }
        lock.lock();
        if (broken) {
          state->broken = true;
          return;
        }
        continue;
      }
      std::optional<Clock::time_point> until;
      if (state->changed) {
        until = state->lastChange + timing.settleAfterLastNotice;
      } else if (recheckAt) {
        until = *recheckAt;
      }
      const Clock::time_point askedListDue = lastListAt + kListAgainMergeWindow;
      if (state->listAsked && (!until || askedListDue < *until)) until = askedListDue;
      clock.wait(lock, state->cv, until);
    }
  });

  int exit = kExitOk;
  ipc::FrameReader reader(in);
  for (;;) {
    auto frame = reader.next();
    if (!frame) {
      logLine(ipc::LogLevel::error, "host's stream broke: " + frame.error().message);
      exit = kExitFailed;
      break;
    }
    if (!frame->has_value()) break;
    auto envelope = ipc::decodeEnvelope(**frame);
    std::optional<uint64_t> id;
    if (envelope) {
      if (envelope->requestId != 0 && !std::holds_alternative<ipc::Refusal>(envelope->body)) id = envelope->requestId;
    } else if (envelope.error().requestId) {
      id = *envelope.error().requestId;
    }
    if (!id) {
      if (envelope && std::holds_alternative<ipc::ListAgain>(envelope->body)) {
        {
          std::lock_guard lock(state->mutex);
          if (state->listAsked) continue;
          state->listAsked = true;
        }
        state->cv.notify_all();
      }
      continue;
    }
    ipc::Refusal refusal{"this driver was started with --watch: it reports devices as they come and go, and answers "
                         "no requests (start it with --device <id> to drive device)",
                         0};
    if (auto sent = out.send(ipc::Envelope{*id, std::move(refusal)}); !sent && sent.error().broken) {
      exit = kExitFailed;
      break;
    }
  }
  {
    std::lock_guard lock(state->mutex);
    state->hostGone = true;
  }
  state->cv.notify_all();
  watcher.join();
  notices.stop();
  out.setConnected(false);
  {
    std::lock_guard lock(state->mutex);
    if (state->broken || state->failure) exit = kExitFailed;
  }
  return exit;
}

}

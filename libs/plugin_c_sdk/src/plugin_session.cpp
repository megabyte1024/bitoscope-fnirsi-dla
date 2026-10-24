/*
 * SPDX-License-Identifier: MIT
 */

#include "plugin_session.h"

#include "declared_abbreviations.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bitoscope_plugin_common.h"
#include "facet_values.h"
#include "frame_output.h"
#include "ipc_frame.h"
#include "ipc_messages.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr unsigned long kMinNetworkPort = 1;
constexpr unsigned long kMaxNetworkPort = 65535;

using Mode = RunMode;

std::optional<Mode> parseInterfaceParams(std::span<const std::string> args, Mode mode) {
  for (std::size_t i = 0; i < args.size();) {
    if (args[i] == "--network-address" && i + 1 < args.size() && !mode.networkAddress) {
      mode.networkAddress = args[i + 1];
      i += 2;
    } else if (args[i] == "--network-port" && i + 1 < args.size() && !mode.networkPort) {
      const std::string& text = args[i + 1];
      if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
      unsigned long port = 0;
      try {
        port = std::stoul(text);
      } catch (...) {
        return std::nullopt;
      }
      if (port < kMinNetworkPort || port > kMaxNetworkPort) return std::nullopt;
      mode.networkPort = static_cast<uint16_t>(port);
      i += 2;
    } else if (args[i] == "--serial-port" && i + 1 < args.size() && !mode.serialPort) {
      mode.serialPort = args[i + 1];
      i += 2;
    } else {
      return std::nullopt;
    }
  }
  return mode;
}

std::string_view interfaceKindName(InterfaceKind kind) {
  switch (kind) {
    case InterfaceKind::usb:
      return "usb";
    case InterfaceKind::network:
      return "network";
    case InterfaceKind::serial:
      return "serial";
    case InterfaceKind::none:
      return "none";
  }
  return "usb";
}

std::string range(uint32_t low, uint32_t high) {
  auto bound = [](uint32_t v) { return v == 0 ? std::string("any") : std::to_string(v); };
  return bound(low) + " to " + bound(high);
}

std::string listed(const std::vector<uint32_t>& values) {
  std::string out;
  for (uint32_t v : values) out += (out.empty() ? "" : ", ") + std::to_string(v);
  return out;
}

std::vector<std::pair<std::string, std::vector<const FacetRegistration*>>> visibleFacets(const bitoscope_device& device,
                                                                                          uint32_t protocol) {
  std::vector<std::pair<std::string, std::vector<const FacetRegistration*>>> out;
  for (const FacetRegistration& registration : device.facets) {
    if (registration.protocolMajor != 0 && registration.protocolMajor != protocol) continue;
    auto group = std::find_if(out.begin(), out.end(), [&](const auto& g) { return g.first == registration.name; });
    if (group == out.end()) {
      out.emplace_back(registration.name, std::vector<const FacetRegistration*>{});
      group = std::prev(out.end());
    }
    auto same = std::find_if(group->second.begin(), group->second.end(),
                             [&](const FacetRegistration* r) { return r->major == registration.major; });
    if (same == group->second.end()) {
      group->second.push_back(&registration);
    } else if ((*same)->protocolMajor == 0) {
      *same = &registration;
    }
  }
  for (auto& group : out) {
    std::sort(group.second.begin(), group.second.end(),
              [](const FacetRegistration* a, const FacetRegistration* b) { return a->major < b->major; });
  }
  return out;
}

std::vector<uint32_t> majorsOf(const std::vector<const FacetRegistration*>& registrations) {
  std::vector<uint32_t> out;
  for (const FacetRegistration* r : registrations) out.push_back(r->major);
  return out;
}

ipc::FacetInfo usableInfo(const bitoscope_device& device, const FacetRegistration& r, std::vector<uint32_t> majors) {
  return ipc::FacetInfo{r.name, ipc::Version{r.major, r.minor}, r.info, r.capabilities, {}, std::move(majors),
                        device.propertiesOf(r.name)};
}

std::vector<ipc::FacetInfo> listedFacets(const bitoscope_device& device) {
  std::vector<std::pair<std::string, std::vector<const FacetRegistration*>>> groups;
  for (const FacetRegistration& r : device.facets) {
    auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == r.name; });
    if (group == groups.end()) {
      groups.emplace_back(r.name, std::vector<const FacetRegistration*>{});
      group = std::prev(groups.end());
    }
    group->second.push_back(&r);
  }
  std::vector<ipc::FacetInfo> out;
  for (auto& [name, registrations] : groups) {
    std::sort(registrations.begin(), registrations.end(),
              [](const FacetRegistration* a, const FacetRegistration* b) { return a->major < b->major; });
    std::vector<uint32_t> majors = majorsOf(registrations);
    majors.erase(std::unique(majors.begin(), majors.end()), majors.end());
    out.push_back(usableInfo(device, *registrations.back(), std::move(majors)));
  }
  return out;
}

class Session {
 public:
  Session(FrameOutput& out, const ServedDevice& served, const SessionOptions& options)
      : out_(out),
        device_(served.device),
        deviceError_(served.refusal),
        unavailable_(served.unavailable),
        identified_(served.identified),
        options_(options) {
    if (device_ != nullptr) {
      for (const FacetPropertyTree& tree : device_->properties) values_[tree.facet] = defaultValues(tree.tree);
    }
  }

  ~Session() { abandon(); }

  int run(ipc::ByteSource& in) {
    ipc::FrameReader reader(in);
    for (;;) {
      auto frame = reader.next();
      if (!frame) {
        logLine(ipc::LogLevel::error, "host's stream broke: " + frame.error().message);
        return kExitFailed;
      }
      if (!frame->has_value()) {
        return handshaken_ || deviceError_.empty() ? kExitOk : kExitFailed;
      }
      if (auto exit = handleFrame(**frame)) return *exit;
    }
  }

 private:
  struct SessionFacet {
    const FacetRegistration* registration = nullptr;
    std::string unusable;
  };

  void abandon() {
    for (auto& [name, handler] : handlers_) handler->abandon();
    handlers_.clear();
    out_.setConnected(false);
  }

  FacetHandler& handlerOf(const FacetRegistration& registration) {
    std::unique_ptr<FacetHandler>& handler = handlers_[registration.name];
    if (!handler) handler = registration.makeHandler(values_[registration.name]);
    return *handler;
  }

  std::optional<int> send(const ipc::Envelope& envelope) {
    auto sent = out_.send(envelope);
    if (sent) return std::nullopt;
    if (sent.error().broken) {
      logLine(ipc::LogLevel::error, "host cannot be written to: " + sent.error().message);
      return kExitFailed;
    }
    if (envelope.requestId != 0 && !std::holds_alternative<ipc::Refusal>(envelope.body)) {
      return refuse(envelope.requestId, "answer could not be sent: " + sent.error().message);
    }
    return std::nullopt;
  }

  std::optional<int> refuse(uint64_t requestId, std::string message, uint16_t member = 0) {
    return send(ipc::Envelope{requestId, ipc::Refusal{std::move(message), member}});
  }

  std::optional<int> handleFrame(std::span<const uint8_t> bytes) {
    auto envelope = ipc::decodeEnvelope(bytes);
    if (!envelope) {
      const ipc::DecodeError& error = envelope.error();
      return refuse(error.requestId.value_or(0), "this plugin cannot act on this message: " + error.message,
                    error.kind == ipc::DecodeErrorKind::unknownMember ? error.member : uint16_t{0});
    }
    const uint64_t id = envelope->requestId;
    if (auto* handshake = std::get_if<ipc::Handshake>(&envelope->body)) return onHandshake(id, *handshake);
    if (auto* command = std::get_if<ipc::FacetCommand>(&envelope->body)) {
      if (!handshaken_) return refuse(id, "no handshake yet: session starts with handshake");
      return onCommand(id, *command);
    }
    if (auto* set = std::get_if<ipc::props::SetProperties>(&envelope->body)) {
      if (!handshaken_) return refuse(id, "no handshake yet: session starts with handshake");
      return onSetProperties(id, *set);
    }
    if (std::holds_alternative<ipc::Refusal>(envelope->body)) return std::nullopt;
    if (id == 0 && std::holds_alternative<ipc::ListAgain>(envelope->body)) return std::nullopt;
    return refuse(id, "this message is not request plugin answers (envelope member " +
                          std::to_string(envelope->body.index() + 1) + ")");
  }

  std::optional<int> onHandshake(uint64_t id, const ipc::Handshake& handshake) {
    if (handshaken_) return refuse(id, "handshake was already made in this session");
    ipc::HandshakeReply reply;
    reply.minor = ipc::kProtocolMinor;
    reply.sdkVersion = BITOSCOPE_PLUGIN_SDK_VERSION;
    reply.pluginVersion = options_.pluginVersion;
    reply.abbreviations = declaredAbbreviations();
    if (!deviceError_.empty()) {
      reply.message = deviceError_;
      reply.unavailable = unavailable_;
      return refused(id, std::move(reply));
    }

    std::set<uint32_t> majors;
    for (uint32_t m = options_.minProtocolMajor; m <= options_.maxProtocolMajor && m != 0; ++m) majors.insert(m);
    for (const FacetRegistration& r : device_->facets) {
      if (r.protocolMajor != 0) majors.insert(r.protocolMajor);
    }
    std::optional<uint32_t> chosen;
    for (uint32_t m : majors) {
      if (m >= handshake.minMajor && m <= handshake.maxMajor) chosen = m;
    }
    if (!chosen) {
      const uint32_t low = majors.empty() ? 0 : *majors.begin();
      const uint32_t high = majors.empty() ? 0 : *majors.rbegin();
      reply.message = "no protocol version in common: host speaks versions " +
                      range(handshake.minMajor, handshake.maxMajor) + " and this plugin " + range(low, high);
      if (handshake.minMajor > high) {
        reply.message += "; host is newer - rebuild plugin with newer bitoscope plugin SDK";
      } else if (handshake.maxMajor < low) {
        reply.message += "; host is older - update host";
      }
      return refused(id, std::move(reply));
    }

    reply.accepted = true;
    reply.major = *chosen;
    reply.device = ipc::DeviceInfo{device_->id, device_->name, selectFacets(*chosen, handshake.requestedFacets),
                                   device_->serialNumber};
    reply.device.identified = identified_;
    handshaken_ = true;
    out_.setDeviceId(device_->id);
    out_.setConnected(true);
    return send(ipc::Envelope{id, std::move(reply)});
  }

  std::optional<int> refused(uint64_t id, ipc::HandshakeReply reply) {
    logLine(ipc::LogLevel::error, "host was refused: " + reply.message);
    if (auto exit = send(ipc::Envelope{id, std::move(reply)})) return exit;
    return kExitFailed;
  }

  std::vector<ipc::FacetInfo> selectFacets(uint32_t protocol, const std::vector<ipc::FacetRequest>& requested) {
    const auto groups = visibleFacets(*device_, protocol);
    std::vector<ipc::FacetInfo> out;
    auto use = [&](const FacetRegistration& r, std::vector<uint32_t> majors) {
      facets_[r.name] = SessionFacet{&r, {}};
      out.push_back(usableInfo(*device_, r, std::move(majors)));
    };
    if (requested.empty()) {
      for (const auto& [name, registrations] : groups) use(*registrations.back(), majorsOf(registrations));
      return out;
    }
    for (const ipc::FacetRequest& request : requested) {
      if (facets_.count(request.name) != 0) continue;
      const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == request.name; });
      if (group == groups.end()) continue;
      const auto& registrations = group->second;
      const FacetRegistration* best = nullptr;
      for (const FacetRegistration* r : registrations) {
        if ((request.minMajor == 0 || r->major >= request.minMajor) &&
            (request.maxMajor == 0 || r->major <= request.maxMajor)) {
          best = r;
        }
      }
      if (best != nullptr) {
        use(*best, majorsOf(registrations));
        continue;
      }
      const FacetRegistration& highest = *registrations.back();
      std::string reason = "this plugin implements " + request.name + " major versions " +
                           listed(majorsOf(registrations)) + "; host asked for " +
                           range(request.minMajor, request.maxMajor);
      facets_[request.name] = SessionFacet{&highest, reason};
      out.push_back(ipc::FacetInfo{highest.name, ipc::Version{highest.major, highest.minor}, {}, {}, std::move(reason),
                                   majorsOf(registrations)});
    }
    return out;
  }

  std::optional<int> onCommand(uint64_t id, const ipc::FacetCommand& command) {
    const auto found = facets_.find(command.facet);
    if (found == facets_.end()) {
      std::string names;
      for (const auto& [name, facet] : facets_) names += (names.empty() ? "" : ", ") + name;
      return refuse(id, "this session has no facet \"" + command.facet + "\" (it has: " +
                            (names.empty() ? std::string("none") : names) + ")");
    }
    const SessionFacet& facet = found->second;
    if (!facet.unusable.empty()) {
      return refuse(id, "facet \"" + command.facet + "\" is not usable in this session: " + facet.unusable);
    }
    FacetAnswer answer = handlerOf(*facet.registration).handle(command.payload);
    if (auto* reply = std::get_if<ipc::FacetReply>(&answer)) return send(ipc::Envelope{id, std::move(*reply)});
    return send(ipc::Envelope{id, std::get<ipc::Refusal>(std::move(answer))});
  }

  bool capturing() const {
    return std::any_of(handlers_.begin(), handlers_.end(), [](const auto& h) { return h.second->capturing(); });
  }

  std::optional<int> onSetProperties(uint64_t id, const ipc::props::SetProperties& set) {
    const auto answer = [&](bool ok, std::string message, std::string property) {
      return send(ipc::Envelope{id, ipc::FacetReply{ok, std::move(message), {}, std::move(property)}});
    };
    const auto found = facets_.find(set.facet);
    if (found == facets_.end()) {
      return answer(false, "setProperties refused: this session has no facet \"" + set.facet + "\"", {});
    }
    if (!found->second.unusable.empty()) {
      return answer(false,
                    "setProperties refused: facet \"" + set.facet + "\" is not usable in this session: " +
                        found->second.unusable,
                    {});
    }
    const bool running = capturing();
    if (auto checked = checkValues(set.facet, device_->propertiesOf(set.facet), set.values, running); !checked) {
      return answer(false, std::move(checked.error().message), std::move(checked.error().propertyId));
    }
    if (set.values.empty()) return answer(true, "no properties to set", {});

    if (auto applied = handlerOf(*found->second.registration).apply(set.values, running); !applied) {
      const ApplyRefusal& refusal = applied.error();
      const bool named = std::any_of(set.values.begin(), set.values.end(), [&](const ipc::props::PropertyValue& v) {
        return !refusal.propertyId.empty() && v.id == refusal.propertyId;
      });
      return answer(false,
                    "setProperties refused by driver: " +
                        (refusal.message.empty() ? std::string("facet \"" + set.facet + "\" refused values")
                                                 : refusal.message),
                    named ? refusal.propertyId : set.values.front().id);
    }
    takeValues(values_[set.facet], set.values);
    return answer(true, "properties set", {});
  }

  FrameOutput& out_;
  const bitoscope_device* device_;
  std::string deviceError_;
  ipc::UnavailableReason unavailable_;
  bool identified_;
  SessionOptions options_;
  bool handshaken_ = false;
  std::map<std::string, SessionFacet> facets_;
  std::map<std::string, std::vector<ipc::props::PropertyValue>> values_;
  std::map<std::string, std::unique_ptr<FacetHandler>> handlers_;
};

int callList(const ListDevices& list, bitoscope_devices& devices) {
  try {
    return list(devices);
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

}

std::vector<ipc::DeviceInfo> describeDevices(const bitoscope_devices& devices) {
  std::vector<ipc::DeviceInfo> out;
  for (const auto& device : devices.devices) {
    out.push_back(ipc::DeviceInfo{device->id, device->name, listedFacets(*device), device->serialNumber});
  }
  return out;
}

std::string listFailed(int result, const bitoscope_devices& devices) {
  if (!devices.listProblem.empty()) return devices.listProblem;
  return "driver could not list its devices (its list function returned " + std::to_string(result) + ")";
}

std::optional<RunMode> parseRunMode(std::span<const std::string> args) {
  if (args.size() == 1 && args[0] == "--interface") return Mode{Mode::Kind::interface, {}};
  if (!args.empty() && args[0] == "--watch") return parseInterfaceParams(args.subspan(1), Mode{Mode::Kind::watch, {}});
  if (!args.empty() && args[0] == "--list") return parseInterfaceParams(args.subspan(1), Mode{Mode::Kind::list, {}});
  if (args.size() >= 2 && args[0] == "--device" && !args[1].empty()) {
    return parseInterfaceParams(args.subspan(2), Mode{Mode::Kind::device, args[1]});
  }
  return std::nullopt;
}

void printUsage() {
  std::fprintf(
      stderr,
      "usage: <driver> --interface     print which interface this driver's devices connect through\n"
      "       <driver> --list [--network-address <addr>] [--network-port <port>] [--serial-port <name>]\n"
      "                        print devices this driver can drive (for bitoscope host)\n"
      "       <driver> --device <id> [--network-address <addr>] [--network-port <port>] [--serial-port <name>]\n"
      "                        drive that device; bitoscope host starts drivers this way\n"
      "       <driver> --watch [--network-address <addr>] [--network-port <port>] [--serial-port <name>]\n"
      "                        report devices as they are plugged in and unplugged\n");
  std::fflush(stderr);
}

int answerInterface(ipc::ByteSink& out, InterfaceKind kind) {
  const std::string line = std::string(interfaceKindName(kind)) + "\n";
  auto sent = out.write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(line.data()), line.size()));
  if (!sent) {
    logLine(ipc::LogLevel::error, "--interface could not be written: " + sent.error());
    return kExitFailed;
  }
  return kExitOk;
}

int serveDevice(ipc::ByteSource& in, FrameOutput& out, const ServedDevice& served, const SessionOptions& options) {
  Session session(out, served, options);
  return session.run(in);
}

int runSession(std::span<const std::string> args, ipc::ByteSource& in, ipc::ByteSink& out, const ListDevices& list,
               const SessionOptions& options) {
  const auto mode = parseRunMode(args);
  if (!mode) {
    printUsage();
    return kExitUsage;
  }
  if (mode->kind == Mode::Kind::interface) return answerInterface(out, options.interfaceKind);
  if (!list) {
    logLine(ipc::LogLevel::error, options.runFunction + "() was given no list function");
    return kExitFailed;
  }
  FrameOutput output(out);
  ActiveOutput active(output);
  if (!active.held()) {
    logLine(ipc::LogLevel::error,
            "another plugin SDK session is already active in this process (one SDK loop per process)");
    return kExitFailed;
  }
  bitoscope_devices devices;
  const int listResult = callList(list, devices);

  if (mode->kind == Mode::Kind::watch) {
    if (listResult != BITOSCOPE_OK) {
      logLine(ipc::LogLevel::error, listFailed(listResult, devices));
      return kExitFailed;
    }
    logLine(ipc::LogLevel::error, "this driver cannot watch for devices");
    return kExitFailed;
  }

  if (mode->kind == Mode::Kind::list) {
    if (listResult != BITOSCOPE_OK) {
      logLine(ipc::LogLevel::error, listFailed(listResult, devices));
      return kExitFailed;
    }
    ipc::DeviceList listed;
    listed.devices = describeDevices(devices);
    listed.pluginVersion = devices.pluginVersion;
    if (auto sent = output.send(ipc::Envelope{0, std::move(listed)}); !sent) {
      logLine(ipc::LogLevel::error, "device list could not be written: " + sent.error().message);
      return kExitFailed;
    }
    return kExitOk;
  }

  const bitoscope_device* device = nullptr;
  std::string deviceError;
  if (listResult != BITOSCOPE_OK) {
    deviceError = listFailed(listResult, devices);
  } else {
    std::string ids;
    for (const auto& d : devices.devices) {
      if (d->id == mode->deviceId) device = d.get();
      ids += (ids.empty() ? "" : ", ") + d->id;
    }
    if (device == nullptr) {
      deviceError = "no device with id \"" + mode->deviceId + "\": this driver found " +
                    (ids.empty() ? std::string("none") : ids);
    }
  }
  if (!deviceError.empty()) logLine(ipc::LogLevel::error, deviceError);

  ServedDevice served;
  served.device = device;
  served.refusal = std::move(deviceError);
  SessionOptions sessionOptions = options;
  if (!devices.pluginVersion.empty()) sessionOptions.pluginVersion = devices.pluginVersion;
  return serveDevice(in, output, served, sessionOptions);
}

}

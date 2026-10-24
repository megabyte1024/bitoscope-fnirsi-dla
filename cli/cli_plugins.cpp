/*
 * SPDX-License-Identifier: MIT
 */

#include "cli_plugins.h"

#include "display_text.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "device_properties.h"
#include "ipc_signal_generator.h"
#include "la_pipe_client.h"
#include "siggen_pipe_client.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bitoscope::cli {

namespace {

std::string pathText(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

std::vector<std::string> idsOf(const ipc::DeviceList& list) {
  std::vector<std::string> ids;
  for (const ipc::DeviceInfo& device : list.devices) ids.push_back(device.id);
  return ids;
}

std::string formatRate(uint64_t milli_hz) {
  struct Unit {
    uint64_t milli_hz;
    const char* name;
  };
  static constexpr Unit kUnits[] = {{1'000'000'000'000, "GHz"}, {1'000'000'000, "MHz"}, {1'000'000, "kHz"}, {1'000, "Hz"}};
  for (const Unit& unit : kUnits) {
    if (milli_hz >= unit.milli_hz && milli_hz % unit.milli_hz == 0) {
      return std::to_string(milli_hz / unit.milli_hz) + " " + unit.name;
    }
  }
  return std::to_string(milli_hz) + " mHz";
}

std::string versionText(const ipc::Version& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor);
}

const char* levelName(ipc::LogLevel level) {
  switch (level) {
    case ipc::LogLevel::debug:
      return "DEBUG";
    case ipc::LogLevel::info:
      return "INFO";
    case ipc::LogLevel::warning:
      return "WARN";
    case ipc::LogLevel::error:
      return "ERROR";
    case ipc::LogLevel::unspecified:
      break;
  }
  return "UNKNOWN";
}

namespace props = bitoscope::ipc::props;

std::string_view unitLabel(props::Unit unit) {
  switch (unit) {
    case props::Unit::none: return "";
    case props::Unit::volt: return "V";
    case props::Unit::second: return "s";
    case props::Unit::hertz: return "Hz";
    case props::Unit::percent: return "%";
    case props::Unit::samples: return "S";
  }
  return "";
}

std::optional<int64_t> boundValue(const props::Bound& bound) {
  if (const auto* v = std::get_if<int64_t>(&bound)) return *v;
  return std::nullopt;
}

std::string formattedOrUnknown(const props::Property& property, int64_t raw) {
  auto text = bitoscope::device_properties::formatValue(property, props::PropertyValue{property.id, raw});
  return text ? *text : "(value this build does not recognize)";
}

std::string defaultText(const props::Property& property) {
  auto def = bitoscope::device_properties::effectiveDefault(property);
  if (!def) return "(default this build does not recognize)";
  auto text = bitoscope::device_properties::formatValue(property, *def);
  return text ? *text : "(default this build does not recognize)";
}

std::string limitsAndDefaultText(const props::Property& property, const props::Bound& min, const props::Bound& max) {
  const auto mn = boundValue(min);
  const auto mx = boundValue(max);
  std::string limits;
  if (mn.has_value() && mx.has_value()) {
    limits = formattedOrUnknown(property, *mn) + " to " + formattedOrUnknown(property, *mx);
  } else if (mn.has_value()) {
    limits = "at least " + formattedOrUnknown(property, *mn);
  } else if (mx.has_value()) {
    limits = "at most " + formattedOrUnknown(property, *mx);
  } else {
    limits = "no limit";
  }
  return limits + ", default " + defaultText(property);
}

std::string propertyTypeText(const props::Property& property) {
  const std::string_view unit = unitLabel(property.unit);
  const std::string unit_suffix = unit.empty() ? std::string() : ", unit " + std::string(unit);
  return std::visit(
      [&](const auto& spec) -> std::string {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec>) {
          return "integer" + unit_suffix + ", " + limitsAndDefaultText(property, spec.min, spec.max);
        } else if constexpr (std::is_same_v<T, props::DecimalSpec>) {
          return "decimal" + unit_suffix + ", " + std::to_string(spec.digits) +
                (spec.digits == 1 ? " digit, " : " digits, ") + limitsAndDefaultText(property, spec.min, spec.max);
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          const std::string style = spec.style == props::BooleanStyle::enableDisable ? "enabled/disabled" : "on/off";
          return "boolean (" + style + "), default " + defaultText(property);
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          std::string choices;
          for (size_t i = 0; i < spec.items.size(); ++i) {
            auto text = bitoscope::device_properties::formatValue(
                property, props::PropertyValue{property.id, props::EnumIndex{static_cast<uint32_t>(i)}});
            choices += (choices.empty() ? "" : ", ") + (text ? *text : std::string("(unrecognized)"));
          }
          return "enumeration, choices: " + (choices.empty() ? std::string("none") : choices) + ", default " +
                defaultText(property);
        } else {
          return std::string("property type this build does not recognize");
        }
      },
      property.type);
}

void appendPropertyNodes(const std::vector<props::PropertyNode>& nodes, const std::string& indent, std::string& out) {
  for (const props::PropertyNode& node : nodes) {
    if (const auto* group = std::get_if<props::PropertyGroup>(&node.node)) {
      out += indent + display_text::displayText(group->name) + ":\n";
      appendPropertyNodes(group->nodes, indent + "  ", out);
      continue;
    }
    if (const auto* property = std::get_if<props::Property>(&node.node)) {
      out += indent + display_text::displayText(property->name) + " [" + property->id + "]: " + propertyTypeText(*property) + ", " +
            (property->changeableDuringCapture ? "changeable during capture" : "fixed during capture") + "\n";
      if (!property->help.empty()) out += indent + "  " + display_text::displayText(property->help) + "\n";
      continue;
    }
    out += indent + "(property node this build does not recognize)\n";
  }
}

std::string rateCeilingLines(const std::vector<uint64_t>& ceilings, const std::string& indent) {
  std::string out;
  size_t i = 0;
  while (i < ceilings.size()) {
    size_t j = i;
    while (j + 1 < ceilings.size() && ceilings[j + 1] == ceilings[i]) ++j;
    const std::string range = (i == j) ? std::to_string(i + 1) : std::to_string(i + 1) + "-" + std::to_string(j + 1);
    out += indent + range + (i == j ? " channel: up to " : " channels: up to ") + formatRate(ceilings[i]) + "\n";
    i = j + 1;
  }
  return out;
}

}

std::filesystem::path defaultPluginDirectory(const std::filesystem::path& executable) {
  return executable.parent_path() / "drivers";
}

std::filesystem::path currentExecutable(std::string_view argv0) {
#ifdef _WIN32
  (void)argv0;
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return {};
    if (n < buffer.size()) {
      buffer.resize(n);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  // WHY: Path of the running executable: verified on Linux only, not verified on macOS.
  std::error_code error;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) return self;
  return std::filesystem::absolute(std::filesystem::path(std::string(argv0)), error);
#endif
}

std::filesystem::path pluginDirectory(std::string_view plugin_dir, const std::filesystem::path& executable) {
  if (!plugin_dir.empty()) return std::filesystem::path(std::u8string(plugin_dir.begin(), plugin_dir.end()));
  return defaultPluginDirectory(executable);
}

std::expected<std::string, std::string> selectDriver(const std::vector<std::string>& drivers,
                                                     std::string_view requested,
                                                     const std::filesystem::path& directory) {
  const std::string where = pathText(directory);
  if (drivers.empty()) {
    return std::unexpected("no drivers in " + where + " (give driver directory with --driver-dir)");
  }
  if (!requested.empty()) {
    if (std::find(drivers.begin(), drivers.end(), requested) != drivers.end()) return std::string(requested);
    return std::unexpected("no driver \"" + std::string(requested) + "\" in " + where + "; there: " + joined(drivers));
  }
  if (drivers.size() > 1) {
    return std::unexpected("several drivers in " + where + " (" + joined(drivers) + "); choose one with --driver");
  }
  return drivers.front();
}

std::expected<ipc::DeviceInfo, std::string> selectDevice(const ipc::DeviceList& list, std::string_view driver,
                                                         std::string_view requested) {
  const std::string who = "driver \"" + std::string(driver) + "\"";
  if (list.devices.empty()) {
    return std::unexpected(who + " found no device" +
                           (requested.empty() ? std::string() : " (asked for \"" + std::string(requested) + "\")"));
  }
  if (!requested.empty()) {
    for (const ipc::DeviceInfo& device : list.devices) {
      if (device.id == requested) return device;
    }
    return std::unexpected(who + " has no device \"" + std::string(requested) + "\"; it found: " +
                           joined(idsOf(list)));
  }
  // WHY: Several devices and none named is refused: the first of several would be a guess on hardware.
  if (list.devices.size() > 1) {
    return std::unexpected(who + " found several devices (" + joined(idsOf(list)) + "); choose one with --device");
  }
  return list.devices.front();
}

std::string describeFacet(const ipc::FacetInfo& facet) {
  const std::string head = facet.name + " " + versionText(facet.version);
  if (!facet.unusable.empty()) return head + ": cannot be used by this host - " + facet.unusable;
  if (facet.name == host::pipe::kLaAcquisitionFacetName) {
    auto caps = host::pipe::LaAcquisitionClient::parseCapabilities(facet.info);
    if (!caps) return head + ": " + caps.error();
    std::string text = head + ": " + std::to_string(caps->num_channels) + " channels, ";
    const auto& rates = caps->supported_sample_rates_milli_hz;
    if (!rates.empty()) {
      const auto [lowest, highest] = std::minmax_element(rates.begin(), rates.end());
      text += std::to_string(rates.size()) + (rates.size() == 1 ? " sample rate " : " sample rates from ") +
              formatRate(*lowest) + (rates.size() == 1 ? "" : " to " + formatRate(*highest)) + ", ";
    }
    return text + (caps->bulk_ring ? "writes bulk ring" : "NO bulk ring - it cannot deliver samples to this host");
  }
  if (facet.name == host::pipe::kSiggenFacetName) {
    auto caps = host::pipe::SiggenClient::parseCapabilities(facet.info);
    if (!caps) return head + ": " + caps.error();
    return head + ": " + std::to_string(caps->num_ports) + " outputs (" + joined(caps->supported_waveforms) + ")";
  }
  return head + " (not known to this host, listed only)";
}

namespace {

std::string unavailableWords(const ipc::DeviceInfo& device) {
  switch (device.unavailable) {
    case ipc::UnavailableReason::none:
      return {};
    case ipc::UnavailableReason::busy:
      return "in use by another program";
    case ipc::UnavailableReason::noPermission:
      return "no permission";
    case ipc::UnavailableReason::firmwareLoading:
      return "firmware loading";
    case ipc::UnavailableReason::other:
      return device.unavailableDetail.empty() ? std::string("driver gave no reason") : device.unavailableDetail;
  }
  return "unknown reason (code " + std::to_string(static_cast<unsigned>(device.unavailable)) + ")";
}

std::string listedEntry(const ipc::DeviceInfo& device) {
  std::string text = device.name;
  if (!device.identified) text += " (generic name)";
  if (!device.serialNumber.empty()) text += " (serial " + device.serialNumber + ")";
  if (device.unavailable != ipc::UnavailableReason::none) text += ", unavailable: " + unavailableWords(device);
  return text;
}

}

std::string describeDevices(std::string_view driver, const ipc::DeviceList& list) {
  const std::string name =
      list.pluginVersion.empty() ? std::string(driver) : std::string(driver) + " " + list.pluginVersion;
  if (list.devices.empty()) return name + ": no device found\n";
  std::string text = name + ": " + std::to_string(list.devices.size()) +
                     (list.devices.size() == 1 ? " device\n" : " devices\n");
  for (const ipc::DeviceInfo& device : list.devices) {
    text += "  " + device.id + "  " + listedEntry(device) + "\n";
    for (const ipc::FacetInfo& facet : device.facets) text += "    " + describeFacet(facet) + "\n";
  }
  return text;
}

std::string describeListedDevice(std::string_view driver, const ipc::DeviceInfo& device) {
  return std::string(driver) + "/" + device.id + " now listed as " + listedEntry(device);
}

std::string describeOpenProgress(std::string_view pluginId, const ipc::OpenProgress& progress) {
  std::string words;
  switch (progress.code) {
    case ipc::OpenProgressCode::firmwareLoading:
      words = "firmware loading";
      break;
    case ipc::OpenProgressCode::waitingForDevice:
      words = "waiting for device to come back";
      break;
    case ipc::OpenProgressCode::other:
      break;
  }
  if (words.empty()) {
    words = progress.detail.empty() ? std::string("in progress") : progress.detail;
  } else if (!progress.detail.empty()) {
    words += " (" + progress.detail + ")";
  }
  return std::string(pluginId) + " opening: " + words;
}

std::string describeDeviceProperties(std::string_view driver, const ipc::DeviceInfo& device) {
  std::string text = std::string(driver) + "/" + device.id + "  " + device.name;
  if (!device.serialNumber.empty()) text += " (serial " + device.serialNumber + ")";
  text += "\n";
  for (const ipc::FacetInfo& facet : device.facets) {
    text += "  " + describeFacet(facet) + "\n";
    if (facet.unusable.empty() && facet.name == host::pipe::kLaAcquisitionFacetName) {
      auto caps = host::pipe::LaAcquisitionClient::parseCapabilities(facet.info);
      if (caps) {
        if (!caps->rate_ceilings_milli_hz.empty()) {
          text += "    rate ceilings:\n" + rateCeilingLines(caps->rate_ceilings_milli_hz, "      ");
        }
        if (!caps->channel_presets.empty()) {
          text += "    channel presets:\n";
          for (const auto& preset : caps->channel_presets) {
            text += "      " + std::to_string(preset.channel) + ": " + display_text::displayText(preset.name) +
                   (preset.colorId.empty() ? "" : " (" + preset.colorId + ")") + "\n";
          }
        }
      }
    }
    if (facet.properties.empty()) {
      text += "    properties: none\n";
    } else {
      text += "    properties:\n";
      appendPropertyNodes(facet.properties, "      ", text);
    }
  }
  return text;
}

std::string formatLogLine(std::string_view plugin_id, const ipc::LogEvent& event) {
  std::string line = "[" + std::string(plugin_id) + "]";
  if (!event.source.empty()) line += "[" + event.source + "]";
  return line + "[" + levelName(event.level) + "] " + event.message;
}

bool isErrorLog(const ipc::LogEvent& event) { return event.level == ipc::LogLevel::error; }

void CaptureEvents::onFacetEvent(const ipc::FacetEvent& event) {
  if (event.facet != ipc::la::kFacetName) return;
  auto decoded = ipc::la::decodeEvent(event.payload);
  if (!decoded) return;
  const auto* ended = std::get_if<ipc::la::CaptureEnded>(&*decoded);
  if (ended == nullptr) return;
  std::lock_guard lock(mutex_);
  ended_[ended->captureId] = *ended;
  latest_ = *ended;
}

std::optional<ipc::la::CaptureEnded> CaptureEvents::ended(uint64_t capture_id) const {
  std::lock_guard lock(mutex_);
  const auto it = ended_.find(capture_id);
  if (it == ended_.end()) return std::nullopt;
  return it->second;
}

std::optional<ipc::la::CaptureEnded> CaptureEvents::latest() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

std::string describeCaptureEnd(const ipc::la::CaptureEnded& ended) {
  switch (ended.reason) {
    case ipc::la::EndReason::sampleLimitReached:
      return "sample limit reached";
    case ipc::la::EndReason::failure:
      return "failed: " + ended.message;
    case ipc::la::EndReason::unspecified:
      break;
  }
  return "ended (reason " + std::to_string(static_cast<unsigned>(ended.reason)) + "): " + ended.message;
}

std::expected<host::pipe::PluginDevice, std::string> openDevice(host::PipePluginHost& host, std::string_view plugin,
                                                                std::string_view device,
                                                                const std::filesystem::path& directory) {
  auto driver = selectDriver(host.drivers(), plugin, directory);
  if (!driver) return std::unexpected(driver.error());
  auto list = host.listDevices(*driver);
  if (!list) return std::unexpected("could not list devices of driver \"" + *driver + "\": " + list.error());
  auto picked = selectDevice(*list, *driver, device);
  if (!picked) return std::unexpected(picked.error());
  auto id = host.open(*driver, picked->id);
  if (!id) {
    return std::unexpected("could not start driver \"" + *driver + "\" for device \"" + picked->id +
                           "\": " + id.error().message);
  }
  const auto connection = host.connection(*id);
  if (!connection) return std::unexpected("driver \"" + *id + "\" was opened but is gone");
  auto handshake = connection->device();
  if (!handshake) return std::unexpected("driver \"" + *id + "\" made no handshake");
  return host::pipe::PluginDevice{*id, std::move(*handshake)};
}

}

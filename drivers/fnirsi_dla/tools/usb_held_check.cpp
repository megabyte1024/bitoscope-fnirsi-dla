/*
 * SPDX-License-Identifier: MIT
 */

#include <libusb.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "fnirsi_protocol.h"

namespace {

constexpr uint8_t kDlaInterfaceNumber = 0;

struct UsbIdToCheck {
  uint16_t vendor = 0;
  uint16_t product = 0;
  uint8_t interface_number = 0;
};

struct Options {
  std::vector<UsbIdToCheck> ids;
  uint32_t seconds = 60;
  uint32_t period_ms = 1000;
  bool help = false;
};

enum class Availability { kFree, kBusy, kNoPermission, kOther };

std::string_view availabilityName(Availability availability) {
  switch (availability) {
    case Availability::kFree: return "free";
    case Availability::kBusy: return "busy";
    case Availability::kNoPermission: return "no permission";
    case Availability::kOther: return "other";
  }
  return "?";
}

struct HeldCheck {
  Availability availability = Availability::kOther;
  std::string binding;
  std::string node_access;
  std::string detail;
};

struct ListedDevice {
  UsbIdToCheck id;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::string device_id;
  std::string interface_dir;
  HeldCheck check;
};

struct DeviceTally {
  std::map<Availability, uint32_t> counts;
};

void usage() {
  std::puts(
      "usage: fnirsi_dla_usb_held_check [options]\n"
      "Hardware-in-loop tool, Linux: lists USB devices of given ids WITHOUT opening them and checks\n"
      "each one as SDK USB layer would: driver bound to claimed interface in sysfs (usbfs: busy,\n"
      "other kernel driver: other, none: free) and read/write access to /dev/bus/usb/BBB/DDD (none:\n"
      "no permission). Repeats every period, one line per device per list, with time one list took.\n"
      "  --id VID:PID:IF   USB id (hex) and interface number to check; repeatable\n"
      "                    (default 1a86:5537:0, DLA)\n"
      "  --seconds N       how long to run (default 60)\n"
      "  --period-ms N     time between lists (default 1000)\n"
      "Exit: 0 ran to end, 1 error.");
}

template <typename T>
std::expected<T, std::string> parseNumber(std::string_view text, int base, std::string_view what) {
  T value{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
    return std::unexpected("bad " + std::string(what) + " '" + std::string(text) + "'");
  }
  return value;
}

std::expected<UsbIdToCheck, std::string> parseUsbId(std::string_view text) {
  const size_t first = text.find(':');
  const size_t second = first == std::string_view::npos ? first : text.find(':', first + 1);
  if (second == std::string_view::npos) return std::unexpected("--id wants VID:PID:IF, got '" + std::string(text) + "'");
  constexpr int hex = 16;
  constexpr int decimal = 10;
  auto vendor = parseNumber<uint16_t>(text.substr(0, first), hex, "vendor id");
  if (!vendor) return std::unexpected(vendor.error());
  auto product = parseNumber<uint16_t>(text.substr(first + 1, second - first - 1), hex, "product id");
  if (!product) return std::unexpected(product.error());
  auto interface_number = parseNumber<uint8_t>(text.substr(second + 1), decimal, "interface number");
  if (!interface_number) return std::unexpected(interface_number.error());
  return UsbIdToCheck{*vendor, *product, *interface_number};
}

std::expected<Options, std::string> parseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
      continue;
    }
    if (i + 1 >= argc) return std::unexpected("missing value after " + std::string(arg));
    const std::string_view value = argv[++i];
    constexpr int decimal = 10;
    if (arg == "--id") {
      auto id = parseUsbId(value);
      if (!id) return std::unexpected(id.error());
      options.ids.push_back(*id);
    } else if (arg == "--seconds") {
      auto seconds = parseNumber<uint32_t>(value, decimal, "--seconds");
      if (!seconds) return std::unexpected(seconds.error());
      options.seconds = *seconds;
    } else if (arg == "--period-ms") {
      auto period = parseNumber<uint32_t>(value, decimal, "--period-ms");
      if (!period || *period == 0) return std::unexpected("--period-ms wants positive whole milliseconds");
      options.period_ms = *period;
    } else {
      return std::unexpected("unknown option " + std::string(arg));
    }
  }
  if (options.ids.empty()) options.ids.push_back({fnirsi::kVendorId, fnirsi::kProductId, kDlaInterfaceNumber});
  return options;
}

std::string readFirstLine(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

HeldCheck checkHeld(const std::string& device_dir, const std::string& interface_dir, uint8_t bus, uint8_t address) {
  namespace fs = std::filesystem;
  HeldCheck check;
  const fs::path sysfs = "/sys/bus/usb/devices";

  std::array<char, 32> node{};
  std::snprintf(node.data(), node.size(), "/dev/bus/usb/%03u/%03u", unsigned{bus}, unsigned{address});
  if (::access(node.data(), R_OK | W_OK) == 0) {
    check.node_access = "rw";
  } else {
    check.node_access = errno == ENOENT ? "missing" : "no access";
  }

  std::error_code ec;
  if (!fs::is_directory(sysfs / device_dir, ec)) {
    check.binding = "?";
    check.detail = "no sysfs entry " + device_dir;
    return check;
  }
  if (!fs::is_directory(sysfs / interface_dir, ec)) {
    check.binding = "?";
    check.detail = "no interface " + interface_dir + " (configuration " +
                   readFirstLine(sysfs / device_dir / "bConfigurationValue") + ")";
    return check;
  }
  const fs::path driver_link = fs::read_symlink(sysfs / interface_dir / "driver", ec);
  check.binding = ec ? std::string("none") : driver_link.filename().string();

  if (check.binding == "usbfs") {
    check.availability = Availability::kBusy;
  } else if (check.node_access != "rw") {
    check.availability = check.node_access == "missing" ? Availability::kOther : Availability::kNoPermission;
    if (check.node_access == "missing") check.detail = std::string("no device node ") + node.data();
  } else if (check.binding != "none") {
    check.availability = Availability::kOther;
    check.detail = "bound to " + check.binding;
  } else {
    check.availability = Availability::kFree;
  }
  return check;
}

std::expected<std::vector<ListedDevice>, std::string> listDevices(libusb_context* ctx,
                                                                  const std::vector<UsbIdToCheck>& ids) {
  libusb_device** list = nullptr;
  const ssize_t count = libusb_get_device_list(ctx, &list);
  if (count < 0) {
    return std::unexpected(std::string("libusb_get_device_list failed: ") + libusb_error_name(static_cast<int>(count)));
  }
  std::vector<ListedDevice> devices;
  for (ssize_t i = 0; i < count; ++i) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS) continue;
    for (const UsbIdToCheck& id : ids) {
      if (descriptor.idVendor != id.vendor || descriptor.idProduct != id.product) continue;
      ListedDevice device;
      device.id = id;
      device.bus = libusb_get_bus_number(list[i]);
      device.address = libusb_get_device_address(list[i]);
      std::array<uint8_t, 7> ports{};
      const int port_count = libusb_get_port_numbers(list[i], ports.data(), static_cast<int>(ports.size()));
      std::string port_path;
      for (int p = 0; p < port_count; ++p) {
        if (p > 0) port_path += '.';
        port_path += std::to_string(ports[static_cast<size_t>(p)]);
      }
      const std::string bus = std::to_string(device.bus);
      const std::string device_dir = port_count > 0 ? bus + "-" + port_path : "usb" + bus;
      device.device_id = "usb-" + bus + "-" + (port_count > 0 ? port_path : std::string("0"));
      const std::string configuration =
          readFirstLine(std::filesystem::path("/sys/bus/usb/devices") / device_dir / "bConfigurationValue");
      device.interface_dir = (port_count > 0 ? device_dir : bus + "-0") + ":" +
                             (configuration.empty() ? std::string("?") : configuration) + "." +
                             std::to_string(id.interface_number);
      device.check = checkHeld(device_dir, device.interface_dir, device.bus, device.address);
      devices.push_back(std::move(device));
    }
  }
  constexpr int unrefDevices = 1;
  libusb_free_device_list(list, unrefDevices);
  return devices;
}

std::string wallClockNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
  localtime_r(&seconds, &local);
  std::array<char, 32> text{};
  const size_t length = std::strftime(text.data(), text.size(), "%H:%M:%S", &local);
  std::snprintf(text.data() + length, text.size() - length, ".%03d", static_cast<int>(millis));
  return text.data();
}

}

int main(int argc, char** argv) {
  auto parsed = parseOptions(argc, argv);
  if (!parsed) {
    std::fprintf(stderr, "FAIL: %s\n\n", parsed.error().c_str());
    usage();
    return 1;
  }
  if (parsed->help) {
    usage();
    return 0;
  }
  const Options& options = *parsed;

  libusb_context* ctx = nullptr;
  if (const int rc = libusb_init(&ctx); rc != LIBUSB_SUCCESS) {
    std::fprintf(stderr, "FAIL: libusb_init failed: %s\n", libusb_error_name(rc));
    return 1;
  }

  using Clock = std::chrono::steady_clock;
  const auto period = std::chrono::milliseconds(options.period_ms);
  const auto start = Clock::now();
  const auto end = start + std::chrono::seconds(options.seconds);
  std::map<std::string, DeviceTally> tallies;
  std::vector<double> list_ms;
  uint32_t empty_lists = 0;

  for (uint64_t list_index = 0;; ++list_index) {
    const auto due = start + period * list_index;
    if (due > end) break;
    std::this_thread::sleep_until(due);

    const auto before = Clock::now();
    auto devices = listDevices(ctx, options.ids);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    if (!devices) {
      std::fprintf(stderr, "FAIL: %s\n", devices.error().c_str());
      libusb_exit(ctx);
      return 1;
    }
    list_ms.push_back(elapsed_ms);
    const std::string now = wallClockNow();
    if (devices->empty()) {
      ++empty_lists;
      std::printf("%s list %llu: no device of given ids, list %.3f ms\n", now.c_str(),
                  static_cast<unsigned long long>(list_index), elapsed_ms);
    }
    for (const ListedDevice& device : *devices) {
      ++tallies[device.device_id].counts[device.check.availability];
      std::printf("%s list %llu: %s %04x:%04x bus %u address %u interface %s: %s (binding %s, node %s%s%s), "
                  "list %.3f ms\n",
                  now.c_str(), static_cast<unsigned long long>(list_index), device.device_id.c_str(),
                  unsigned{device.id.vendor}, unsigned{device.id.product}, unsigned{device.bus},
                  unsigned{device.address}, device.interface_dir.c_str(),
                  std::string(availabilityName(device.check.availability)).c_str(), device.check.binding.c_str(),
                  device.check.node_access.c_str(), device.check.detail.empty() ? "" : ", ",
                  device.check.detail.c_str(), elapsed_ms);
    }
    std::fflush(stdout);
  }
  libusb_exit(ctx);

  std::printf("SUMMARY lists %zu, lists with no device %u\n", list_ms.size(), empty_lists);
  for (const auto& [device_id, tally] : tallies) {
    std::printf("SUMMARY %s:", device_id.c_str());
    for (const Availability availability :
         {Availability::kFree, Availability::kBusy, Availability::kNoPermission, Availability::kOther}) {
      const auto found = tally.counts.find(availability);
      std::printf(" %s %u", std::string(availabilityName(availability)).c_str(),
                  found == tally.counts.end() ? 0U : found->second);
    }
    std::printf("\n");
  }
  if (!list_ms.empty()) {
    std::vector<double> sorted = list_ms;
    std::ranges::sort(sorted);
    std::printf("SUMMARY list time ms: min %.3f median %.3f max %.3f\n", sorted.front(), sorted[sorted.size() / 2],
                sorted.back());
  }
  return 0;
}

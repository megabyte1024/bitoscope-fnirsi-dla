/*
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "console_code_page.h"
#include "fnirsi_device.h"
#include "fnirsi_protocol.h"
#include "libusb_bus.h"
#include "libusb_stream_transport.h"
#include "raw_lanes.h"
#include "soak_plan.h"
#include "usb_inventory.h"
#include "usb_stream.h"

namespace {

struct Options {
  std::string id = "usb-2-2";
  uint64_t rate_hz = 1'000'000;
  uint32_t mask = 0x1;
  uint64_t mib = 16;
  double timeout_s = 10;
  bool drain = false;
  uint32_t transfer_kib = 1024;
  uint32_t transfers = 8;
  int32_t threshold_mv = 1600;
  uint32_t tolerance = 2;
  uint64_t skip_bytes = 65536;
  bool help = false;
};

void usage() {
  std::puts(
      "usage: fnirsi_dla_raw_probe [options]\n"
      "Asks DLA for RAW request (no driver rules) and reads bytes back; PWM of P0-P3 must be\n"
      "running into D0-D3 (bitoscope_cli --pwm-start), and fnirsi_dla driver must not be running.\n"
      "  --id ID           device id, as fnirsi_dla lists it, default usb-2-2\n"
      "  --rate-hz N       sample rate, default 1000000\n"
      "  --mask M          RAW device channel mask sent to device, decimal or 0x hex, default 0x1\n"
      "  --mib N           stop after N MiB of stream, default 16\n"
      "  --timeout-s S     give up on stream after S seconds, default 10\n"
      "  --drain           drain bulk-IN before starting, as driver does (default: not)\n"
      "  --transfer-kib N  size of each transfer in KiB, default 1024\n"
      "  --transfers N     transfers in flight, default 8\n"
      "  --tolerance N     samples run may differ from median of its level, default 2\n"
      "  --skip-bytes N    channel reading is judged on stream after its first N bytes (default 65536,\n"
      "                    rounded down to multiple of 16); start itself is reported on START lines\n"
      "Exit: 0 claimed reading right, 2 bytes but wrong, 3 no data or stalled, 1 error.");
}

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--help" || a == "-h") {
      o.help = true;
    } else if (a == "--drain") {
      o.drain = true;
    } else if (a == "--id") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.id = v;
    } else if (a == "--rate-hz") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.rate_hz = std::strtoull(v, nullptr, 10);
    } else if (a == "--mask") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.mask = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    } else if (a == "--mib") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.mib = std::strtoull(v, nullptr, 10);
    } else if (a == "--timeout-s") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.timeout_s = std::strtod(v, nullptr);
    } else if (a == "--transfer-kib") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.transfer_kib = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--transfers") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.transfers = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--skip-bytes") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.skip_bytes = std::strtoull(v, nullptr, 10);
    } else if (a == "--tolerance") {
      const char* v = next();
      if (!v) return std::nullopt;
      o.tolerance = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else {
      return std::nullopt;
    }
  }
  if (o.mask == 0 || o.mask > 0xFFFF || o.rate_hz == 0 || o.mib == 0 || o.transfers == 0 || o.transfer_kib == 0) {
    return std::nullopt;
  }
  return o;
}

int matchingInput(uint64_t rate_hz, uint64_t period) {
  for (uint32_t port = 0; port < fnirsi::tools::kRigPorts; ++port) {
    const auto& signal = fnirsi::tools::kDefaultRig[port];
    const auto shape = fnirsi::tools::dlaPwmShape(rate_hz, signal.milli_hz, signal.duty_percent);
    if (!shape) continue;
    const double expected = static_cast<double>(shape->period_num) / static_cast<double>(shape->den);
    const double tolerance = (std::max)(3.0, expected / 200.0);
    if (std::abs(static_cast<double>(period) - expected) <= tolerance) return static_cast<int>(port);
  }
  return -1;
}

std::expected<bitoscope::plugin::FoundDevice, std::string> placeOf(const std::string& id) {
  namespace sdk = bitoscope::plugin_c_sdk;
  auto bus = sdk::makeLibusbBus();
  if (!bus) return std::unexpected(bus.error());
  const sdk::DeclaredUsbId dla{fnirsi::kVendorId, fnirsi::kProductId, fnirsi::kInterfaceNumber};
  auto found = sdk::usbInventory(**bus, std::span<const sdk::DeclaredUsbId>(&dla, 1));
  if (!found) return std::unexpected(found.error());
  std::string ids;
  for (const sdk::FoundUsbDevice& device : *found) {
    if (device.id == id) {
      bitoscope::plugin::FoundDevice unit;
      unit.id = device.id;
      unit.location = bitoscope::plugin::UsbLocation{device.vendorId, device.productId, device.interfaceNumber,
                                                     device.bus,      device.address,   device.ports};
      return unit;
    }
    ids += (ids.empty() ? "" : ", ") + device.id;
  }
  return std::unexpected("no DLA with id " + id + "; found: " + (ids.empty() ? std::string("none") : ids));
}

struct RawStream {
  std::vector<uint8_t> bytes;
  uint64_t chunks = 0;
  uint64_t events = 0;
  std::optional<double> first_chunk_s;
};

void drainSampleEndpoint(bitoscope::plugin_c_sdk::UsbStreamTransport& transport) {
  constexpr uint32_t kDrainTimeoutMs = 20;
  constexpr size_t kDrainMaxReads = 64;
  std::vector<uint8_t> scratch(16 * 1024);
  size_t discarded = 0;
  size_t reads = 0;
  bool capped = true;
  while (reads < kDrainMaxReads) {
    const auto read = transport.readNow(scratch, kDrainTimeoutMs);
    ++reads;
    if (!read || *read == 0) {
      capped = false;
      break;
    }
    discarded += static_cast<size_t>(*read);
  }
  std::printf("drain: %zu byte(s) discarded in %zu read(s)%s\n", discarded, reads, capped ? " (hit its cap)" : "");
}

std::expected<RawStream, std::string> readRawStream(bitoscope::plugin_c_sdk::UsbStreamTransport& transport,
                                                    const Options& o) {
  namespace sdk = bitoscope::plugin_c_sdk;
  if (auto allocated = transport.allocate(o.transfers, o.transfer_kib * 1024); !allocated) {
    return std::unexpected(allocated.error());
  }
  std::deque<uint32_t> order;
  std::vector<bool> done(o.transfers, false);
  std::vector<sdk::TransferDone> results(o.transfers);
  uint32_t flying = 0;
  for (uint32_t t = 0; t < o.transfers; ++t) {
    if (auto submitted = transport.submit(t); !submitted) {
      return std::unexpected("transfer could not be queued: " + submitted.error().name);
    }
    order.push_back(t);
    ++flying;
  }
  RawStream stream;
  const uint64_t limit = o.mib * 1024 * 1024;
  stream.bytes.reserve(static_cast<size_t>(limit));
  const auto started_at = std::chrono::steady_clock::now();
  const auto elapsed_s = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
  };
  std::vector<sdk::TransferDone> completed;
  constexpr uint32_t kPollMs = 20;
  while (stream.bytes.size() < limit && flying > 0 && elapsed_s() <= o.timeout_s) {
    completed.clear();
    transport.waitForCompletions(kPollMs, completed);
    for (const sdk::TransferDone& d : completed) {
      done[d.transfer] = true;
      results[d.transfer] = d;
      --flying;
    }
    while (!order.empty() && done[order.front()]) {
      const uint32_t t = order.front();
      order.pop_front();
      done[t] = false;
      const sdk::TransferDone& d = results[t];
      if (d.status != sdk::TransferStatus::completed) {
        ++stream.events;
        std::printf("stream event: USB transfer ended with %s carrying %llu byte(s); not resubmitted\n",
                    std::string(sdk::transferStatusName(d.status)).c_str(), static_cast<unsigned long long>(d.bytes));
        continue;
      }
      if (!stream.first_chunk_s) stream.first_chunk_s = elapsed_s();
      const auto received = transport.received(t).first(static_cast<size_t>(d.bytes));
      const size_t room = static_cast<size_t>(limit) - stream.bytes.size();
      stream.bytes.insert(stream.bytes.end(), received.begin(),
                          received.begin() + static_cast<std::ptrdiff_t>((std::min)(received.size(), room)));
      ++stream.chunks;
      if (auto submitted = transport.submit(t); !submitted) {
        ++stream.events;
        std::printf("stream event: USB transfer resubmission refused with %s\n", submitted.error().name.c_str());
        continue;
      }
      order.push_back(t);
      ++flying;
    }
  }
  if (stream.bytes.size() < limit) {
    ++stream.events;
    std::printf("stream event: stream stopped after %zu of %llu byte(s), %u transfer(s) in flight, %.1f s\n",
                stream.bytes.size(), static_cast<unsigned long long>(limit), flying, elapsed_s());
  }
  return stream;
}

int run(int argc, char** argv) {
  // WHY: Stdout is line-buffered even when redirected: a run killed by timeout keeps what it printed.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const auto options = parse(argc, argv);
  if (!options || options->help) {
    usage();
    return options ? 0 : 1;
  }
  const Options& o = *options;

  const auto place = placeOf(o.id);
  if (!place) {
    std::fprintf(stderr, "raw_probe: %s\n", place.error().c_str());
    return 1;
  }
  fnirsi::FnirsiDevice device;
  if (auto opened = device.openAt(*place); !opened) {
    std::fprintf(stderr, "raw_probe: open failed: %s\n", opened.error().detail.c_str());
    return 1;
  }
  auto transport = bitoscope::plugin_c_sdk::makeLibusbStreamTransport(
      device.usb().libusbContext(), device.usb().libusbHandle(), fnirsi::kEndpointSampleDataIn);
  if (o.drain) drainSampleEndpoint(*transport);
  const auto setting = fnirsi::buildParameterSetting(o.rate_hz, o.threshold_mv, 0);
  if (!setting) {
    std::fprintf(stderr, "raw_probe: ParameterSetting: %s\n", setting.error().c_str());
    return 1;
  }
  if (auto r = device.sendFramed(fnirsi::FramedCommand::kParameterSetting, *setting); !r) {
    std::fprintf(stderr, "raw_probe: ParameterSetting failed: %s\n", r.error().c_str());
    return 1;
  }
  if (auto r = device.sendFramed(fnirsi::FramedCommand::kSimpleTrigger, fnirsi::buildSimpleTrigger(o.mask)); !r) {
    std::fprintf(stderr, "raw_probe: SimpleTrigger failed: %s\n", r.error().c_str());
    return 1;
  }

  const auto streamed = readRawStream(*transport, o);
  transport.reset();
  (void)device.sendFramed(fnirsi::FramedCommand::kStop, fnirsi::buildStopPayload());
  device.close();
  if (!streamed) {
    std::fprintf(stderr, "raw_probe: stream start failed: %s\n", streamed.error().c_str());
    return 1;
  }
  const std::vector<uint8_t>& stream = streamed->bytes;

  std::printf("PROBE id=%s rate_hz=%llu mask=0x%x channels_in_mask=%d drain=%d transfer_kib=%u transfers=%u\n",
              o.id.c_str(), static_cast<unsigned long long>(o.rate_hz), o.mask, std::popcount(o.mask), o.drain ? 1 : 0,
              o.transfer_kib, o.transfers);
  std::printf("RECEIVED bytes=%zu chunks=%llu stream_events=%llu first_chunk_s=%s\n", stream.size(),
              static_cast<unsigned long long>(streamed->chunks), static_cast<unsigned long long>(streamed->events),
              streamed->first_chunk_s ? std::to_string(*streamed->first_chunk_s).c_str() : "none");
  if (stream.empty()) {
    std::puts("RESULT NO_DATA: device did not stream");
    return 3;
  }

  // WHY: Skip is a multiple of 16 so lanes keep their places under every lane-count reading.
  const size_t skip = std::min<size_t>(stream.size() / 2, static_cast<size_t>(o.skip_bytes) & ~size_t{15});
  const std::span<const uint8_t> whole(stream);
  const std::span<const uint8_t> body = whole.subspan(skip);
  std::printf("BODY skips first %zu byte(s) of %zu\n", skip, stream.size());

  for (const uint32_t group : {1u, 2u, 4u, 8u, 16u}) {
    for (uint32_t lane = 0; lane < group; ++lane) {
      const auto s = fnirsi::tools::analyzeLane(body, group, lane, o.tolerance);
      const int input = s.stable ? matchingInput(o.rate_hz, s.period()) : -1;
      std::printf("LANE group=%u lane=%u samples=%llu edges=%llu high=%llu low=%llu period=%llu stable=%d "
                  "pwm_input=%s first_deviation=%s\n",
                  group, lane, static_cast<unsigned long long>(s.samples), static_cast<unsigned long long>(s.edges),
                  static_cast<unsigned long long>(s.high_median), static_cast<unsigned long long>(s.low_median),
                  static_cast<unsigned long long>(s.period()), s.stable ? 1 : 0,
                  input >= 0 ? ("D" + std::to_string(input)).c_str() : "-",
                  s.first_deviation ? std::to_string(*s.first_deviation).c_str() : "none");
    }
  }

  const uint32_t claimed_group = static_cast<uint32_t>(std::popcount(o.mask));
  uint32_t wired = 0, right = 0, lane = 0;
  for (uint32_t channel = 0; channel < 16; ++channel) {
    if (!(o.mask & (1u << channel))) continue;
    if (channel < fnirsi::tools::kRigPorts) {
      ++wired;
      const auto s = fnirsi::tools::analyzeLane(body, claimed_group, lane, o.tolerance);
      const int input = s.stable ? matchingInput(o.rate_hz, s.period()) : -1;
      const bool ok = input == static_cast<int>(channel);
      const auto start = fnirsi::tools::analyzeLane(whole, claimed_group, lane, o.tolerance);
      std::printf("START channel=D%u lane=%u first_deviation=%s stable_whole=%d\n", channel, lane,
                  start.first_deviation ? std::to_string(*start.first_deviation).c_str() : "none", start.stable ? 1 : 0);
      right += ok ? 1 : 0;
      std::printf("CLAIMED channel=D%u lane=%u of %u stable=%d carries=%s expected=D%u -> %s\n", channel, lane,
                  claimed_group, s.stable ? 1 : 0, input >= 0 ? ("D" + std::to_string(input)).c_str() : "-", channel,
                  ok ? "RIGHT" : "WRONG");
    }
    ++lane;
  }
  if (wired == 0) {
    std::puts("RESULT UNCHECKED: no wired input (D0-D3) in mask");
    return 2;
  }
  if (right == wired && streamed->events == 0) {
    std::printf("RESULT RIGHT: %u of %u wired inputs read back as claimed\n", right, wired);
    return 0;
  }
  if (streamed->events != 0 && right == wired) {
    std::printf("RESULT RIGHT_BUT_STREAM_EVENTS: %u of %u read back, %llu stream event(s)\n", right, wired,
                static_cast<unsigned long long>(streamed->events));
    return 3;
  }
  std::printf("RESULT WRONG: %u of %u wired inputs read back as claimed\n", right, wired);
  return 2;
}

}

int main(int argc, char** argv) {
  return bitoscope::console::runWithUtf8Console(bitoscope::console::systemConsoleCodePage(),
                                               [&] { return run(argc, argv); });
}

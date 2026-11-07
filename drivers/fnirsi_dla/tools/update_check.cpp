/*
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <cstdio>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fnirsi_device.h"
#include "fnirsi_protocol.h"
#include "libusb_bus.h"
#include "usb_inventory.h"
#include "usb_update_transport.h"
#include "wch/wch_route.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kWriteTimeoutMs = 10'000;
constexpr uint32_t kReplyTimeoutMs = 15'000;
constexpr uint32_t kDrainTimeoutMs = 100;
constexpr int kWriteTries = 6;
constexpr auto kResetWait = std::chrono::milliseconds(1'000);

void say(const std::string& line) {
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
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
      unit.location = bitoscope::plugin::toLocation(bitoscope::plugin::UsbLocation{
          device.vendorId, device.productId, device.interfaceNumber, device.bus, device.address, device.ports});
      return unit;
    }
    ids += (ids.empty() ? "" : ", ") + device.id;
  }
  return std::unexpected(std::format("no DLA with id {}; found: {}", id, ids.empty() ? std::string("none") : ids));
}

std::expected<void, std::string> send(fnirsi::UsbUpdateTransport& usb, std::span<const uint8_t> packet,
                                      bool goneAfterWholePacketIsSent = false) {
  for (int tryNumber = 1; tryNumber <= kWriteTries; ++tryNumber) {
    const fnirsi::TransferResult r = usb.writeCommand(packet, kWriteTimeoutMs);
    if (r.status == fnirsi::TransferStatus::kOk && r.transferred == packet.size()) return {};
    if (r.status == fnirsi::TransferStatus::kGone && goneAfterWholePacketIsSent && r.transferred == packet.size()) {
      return {};
    }
    if (r.transferred != 0) return std::unexpected(std::format("write ended part way ({}): {}", r.transferred, r.detail));
    say(std::format("  write not taken ({}), sending again", r.detail));
    if (tryNumber >= 2) usb.clearHalts();
  }
  return std::unexpected(std::string("analyzer took no write"));
}

std::expected<std::vector<uint8_t>, std::string> transact(fnirsi::UsbUpdateTransport& usb,
                                                          std::span<const uint8_t> packet) {
  if (auto sent = send(usb, packet); !sent) return std::unexpected(sent.error());
  std::vector<uint8_t> reply(512);
  const fnirsi::TransferResult r = usb.readReply(reply, kReplyTimeoutMs);
  if (r.status != fnirsi::TransferStatus::kOk) return std::unexpected(std::format("no reply: {}", r.detail));
  reply.resize(r.transferred);
  return reply;
}

void drain(fnirsi::UsbUpdateTransport& usb) {
  std::vector<uint8_t> reply(512);
  for (int i = 0; i < 64; ++i) {
    if (usb.readReply(reply, kDrainTimeoutMs).status != fnirsi::TransferStatus::kOk) return;
  }
}

std::vector<uint8_t> padded(std::initializer_list<uint8_t> head) {
  std::vector<uint8_t> packet(512, 0x00);
  std::copy(head.begin(), head.end(), packet.begin());
  return packet;
}

std::expected<fnirsi::McuVersion, std::string> version(fnirsi::UsbUpdateTransport& usb) {
  if (auto flushed = transact(usb, padded({0x0A, 0x0B})); !flushed) return std::unexpected(flushed.error());
  auto reply = transact(usb, padded({0x0A, 0x81}));
  if (!reply) return std::unexpected(reply.error());
  const auto parsed = fnirsi::parseMcuVersionReply(*reply);
  if (!parsed) return std::unexpected(std::string("version reply does not decode"));
  return *parsed;
}

std::string modeOf(const fnirsi::McuVersion& v) {
  return std::format("{} running, next start {}", v.application_running ? "program" : "bootloader",
                     v.next_start == fnirsi::NextStart::kBootloader ? "bootloader" : "program");
}

std::expected<fnirsi::McuVersion, std::string> resetInto(fnirsi::UsbUpdateTransport& usb, bool bootloader) {
  const std::vector<uint8_t> select{0x0A, 0x89, static_cast<uint8_t>(bootloader ? 0x01 : 0x00), 0, 0, 0, 0, 0, 0, 0};
  auto selected = transact(usb, select);
  if (!selected) return std::unexpected("boot selection: " + selected.error());
  if (auto reset = send(usb, padded({0x0A, 0x90}), true); !reset) return std::unexpected("reset: " + reset.error());
  usb.close();
  std::this_thread::sleep_for(kResetWait);
  const auto started = Clock::now();
  if (auto reopened = usb.reopen(); !reopened) return std::unexpected("reopen: " + reopened.error());
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
  say(std::format("  reopened {} ms after 1 s wait", ms));
  drain(usb);
  return version(usb);
}

int fail(const std::string& why) {
  say("update_check: " + why);
  return 1;
}

}

int main(int argc, char** argv) {
  fnirsi::wch::useWchDriverWhereBound();
  std::string id = "usb-1-15";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--id" && i + 1 < argc) {
      id = argv[++i];
    } else {
      say("usage: fnirsi_dla_update_check [--id ID]\n"
          "  resets DLA into its bootloader and back to its program over firmware update transport;\n"
          "  flashes nothing");
      return arg == "--help" ? 0 : 2;
    }
  }
  auto unit = placeOf(id);
  if (!unit) return fail(unit.error());
  fnirsi::FnirsiDevice device;
  if (auto opened = device.openAt(*unit); !opened) return fail("open: " + opened.error().detail);
  say(std::format("{} opened: {}", id, modeOf(device.connectInfo().mcu_version)));
  fnirsi::UsbUpdateTransport usb(device.handOverUsb(), *unit);
  drain(usb);

  say("reset into bootloader");
  auto inBootloader = resetInto(usb, true);
  if (!inBootloader) return fail(inBootloader.error());
  say("  " + modeOf(*inBootloader));
  if (inBootloader->application_running) return fail("analyzer did not enter its bootloader");

  say("reset into program");
  auto inProgram = resetInto(usb, false);
  if (!inProgram) return fail(inProgram.error());
  say("  " + modeOf(*inProgram));
  if (!inProgram->application_running) return fail("analyzer did not start its program");
  say("round trip done");
  return 0;
}

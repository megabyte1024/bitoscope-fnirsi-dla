/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitoscope_driver_cpp.h"
#include "command_link.h"
#include "connect_sequence.h"
#include "dla_model.h"
#include "fnirsi_protocol.h"
#include "sample_stream_device.h"

namespace fnirsi {

struct ConnectInfo {
  McuVersion mcu_version;
  DeviceData device_data;
};

class FnirsiDevice final : public SampleStreamDevice {
 public:
  FnirsiDevice() = default;
  ~FnirsiDevice();

  FnirsiDevice(const FnirsiDevice&) = delete;
  FnirsiDevice& operator=(const FnirsiDevice&) = delete;

  [[nodiscard]] std::expected<void, bitoscope::plugin::DeviceFailure> openAt(const bitoscope::plugin::FoundDevice& unit);

  [[nodiscard]] std::expected<DlaIdentity, bitoscope::plugin::DeviceFailure> readIdentityAt(
      const bitoscope::plugin::FoundDevice& unit);

  void close();
  [[nodiscard]] bitoscope::plugin::UsbDevice handOverUsb();

  bool isOpen() const { return usb_.isOpen(); }
  bool inBootloader() const { return in_bootloader_; }
  const ConnectInfo& connectInfo() const { return info_; }
  bitoscope::plugin::UsbDevice& usb() { return usb_; }

  [[nodiscard]] std::expected<void, std::string> sendFramed(
      FramedCommand cmd, std::span<const uint8_t> payload) override;

  void streamSamples(bitoscope::plugin::Sink& sink, const bitoscope::plugin::UsbStream& stream) override;

 private:
  [[nodiscard]] std::expected<ConnectInfo, std::string> identify();
  [[nodiscard]] std::expected<void, bitoscope::plugin::DeviceFailure> connectOpened();
  void closeWith(std::span<const Farewell> farewell);

  [[nodiscard]] std::expected<std::vector<uint8_t>, std::string> sendDirect(
      DirectCommand cmd, std::span<const uint8_t> payload, bool read_reply);

  static void logLinkNote(std::string_view note);

  class UsbTransport final : public BulkTransport {
   public:
    explicit UsbTransport(bitoscope::plugin::UsbDevice& usb) : usb_(usb) {}
    std::expected<size_t, TransferFailure> bulkTransfer(uint8_t endpoint, std::span<uint8_t> data,
                                                        unsigned timeout_ms) override;

   private:
    bitoscope::plugin::UsbDevice& usb_;
  };

  bitoscope::plugin::UsbDevice usb_;
  ConnectInfo info_{};
  bool in_bootloader_ = false;
  // WHY: transport_ refers to usb_, so it must be declared after it.
  UsbTransport transport_{usb_};
  CommandLink link_{transport_, &FnirsiDevice::logLinkNote};
};

}

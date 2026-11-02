/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bitoscope_driver_cpp.h"
#include "firmware_file.h"
#include "firmware_flasher.h"
#include "ipc_firmware_update.h"
#include "sample_stream_device.h"
#include "update_transport.h"

namespace fnirsi {

class UpdateGate {
 public:
  void close(std::string reason);
  std::optional<std::string> closedReason() const;

 private:
  mutable std::mutex mutex_;
  std::optional<std::string> reason_;
};

class GatedDevice final : public SampleStreamDevice {
 public:
  GatedDevice(std::shared_ptr<SampleStreamDevice> device, std::shared_ptr<UpdateGate> gate);

  std::expected<void, std::string> sendFramed(FramedCommand cmd, std::span<const uint8_t> payload) override;
  void streamSamples(bitoscope::plugin::Sink& sink, const bitoscope::plugin::UsbStream& stream) override;

 private:
  std::shared_ptr<SampleStreamDevice> device_;
  std::shared_ptr<UpdateGate> gate_;
};

using UpdateTransportFactory = std::function<std::unique_ptr<UpdateTransport>()>;

struct FirmwareUpdateTuning {
  FlashTiming timing;
  FlashSleep sleep;
  std::chrono::milliseconds progress_interval{250};
};

struct FirmwareUpdateSetup {
  bool in_bootloader = false;
  std::string device_name;
  UpdateTransportFactory make_transport;
  FirmwareUpdateTuning tuning;
};

bitoscope::plugin::FacetInfo firmwareUpdateFacetInfo(bool in_bootloader, std::string_view device_name);

class DlaFirmwareUpdate {
 public:
  DlaFirmwareUpdate(FirmwareUpdateSetup setup, std::shared_ptr<UpdateGate> gate);
  ~DlaFirmwareUpdate();
  DlaFirmwareUpdate(const DlaFirmwareUpdate&) = delete;
  DlaFirmwareUpdate& operator=(const DlaFirmwareUpdate&) = delete;

  std::expected<bitoscope::plugin::CommandAnswer, std::string> command(std::span<const uint8_t> payload);

 private:
  using Answer = std::expected<bitoscope::plugin::CommandAnswer, std::string>;
  struct Verdict {
    std::optional<FirmwareFile> file;
    bitoscope::ipc::firmware::CheckReply reply;
  };

  Answer beginUpload(const bitoscope::ipc::firmware::BeginUpload& begin);
  Answer uploadPiece(const bitoscope::ipc::firmware::UploadPiece& piece);
  Answer check(const bitoscope::ipc::firmware::Check& check);
  Answer start(const bitoscope::ipc::firmware::Start& start);
  Answer cancel(const bitoscope::ipc::firmware::Cancel& cancel);
  Answer startProgram();
  Answer status();
  std::optional<std::string> uploadIncomplete(uint32_t upload_id) const;
  Verdict judge(std::string_view named_model, FlashConfirmations confirmed) const;
  void launch(FlashImages images, bool program_only);
  void joinFinishedWorker();
  void onProgress(const FlashProgress& progress);
  void onFinished(const FlashResult& result);

  FirmwareUpdateSetup setup_;
  std::shared_ptr<UpdateGate> gate_;
  mutable std::mutex mutex_;
  uint32_t upload_id_ = 0;
  uint64_t upload_total_ = 0;
  std::string upload_name_;
  std::vector<uint8_t> upload_;
  bool running_ = false;
  std::optional<bitoscope::ipc::firmware::FirmwareUpdateEvent> last_;
  std::chrono::steady_clock::time_point last_progress_sent_{};
  FlashPhase last_phase_ = FlashPhase::kPreparing;
  int last_attempt_ = 0;
  std::unique_ptr<UpdateTransport> transport_;
  std::unique_ptr<FirmwareFlasher> flasher_;
  std::jthread worker_;
};

}

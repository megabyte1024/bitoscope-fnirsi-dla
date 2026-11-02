/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_firmware_update.h"

#include <format>
#include <type_traits>
#include <utility>
#include <variant>

#include "number_text.h"

namespace fnirsi {

namespace {

namespace fw = bitoscope::ipc::firmware;
namespace plugin = bitoscope::plugin;

constexpr uint64_t kUploadLimit = 16ull * 1024 * 1024;
constexpr uint32_t kEstimatedSeconds = 300;
constexpr std::string_view kRunningReason = "firmware update runs on this device";
constexpr std::string_view kRanReason = "firmware update ran on this device: open device again to use it";
constexpr std::string_view kModel16 = "DL16";
constexpr std::string_view kModel32 = "DL32";
constexpr std::string_view kNoTransport = "this device offers no firmware update";

std::string modelText(FirmwareModel model) {
  return std::string(model == FirmwareModel::k16Channels ? kModel16 : kModel32);
}

plugin::CommandAnswer refused(std::string message) { return plugin::CommandAnswer{false, std::move(message), {}}; }

plugin::CommandAnswer answered(std::string message, const std::expected<bitoscope::ipc::Bytes, std::string>& payload) {
  if (!payload) return refused("reply could not be encoded: " + payload.error());
  return plugin::CommandAnswer{true, std::move(message), *payload};
}

fw::Phase phaseOf(FlashPhase phase) {
  switch (phase) {
    case FlashPhase::kPreparing:
      return fw::Phase::preparing;
    case FlashPhase::kMcu:
    case FlashPhase::kFpga:
      return fw::Phase::sending;
    case FlashPhase::kFinishing:
      break;
  }
  return fw::Phase::finishing;
}

std::string imageNameOf(FlashPhase phase) {
  if (phase == FlashPhase::kMcu) return "MCU";
  if (phase == FlashPhase::kFpga) return "FPGA";
  return {};
}

fw::Outcome outcomeOf(FlashOutcome outcome) {
  switch (outcome) {
    case FlashOutcome::kDone:
      return fw::Outcome::done;
    case FlashOutcome::kCancelled:
      return fw::Outcome::cancelled;
    case FlashOutcome::kFailed:
      return fw::Outcome::failed;
    case FlashOutcome::kNeedsReplug:
      break;
  }
  return fw::Outcome::needsReplug;
}

fw::Verdict verdictOf(FitRefusalKind kind) {
  switch (kind) {
    case FitRefusalKind::kAnalyzerModelUnknown:
      return fw::Verdict::modelUnknown;
    case FitRefusalKind::kUnknownFile:
      return fw::Verdict::unknownFile;
    case FitRefusalKind::kOtherModel:
      break;
  }
  return fw::Verdict::otherModel;
}

void publish(const fw::FirmwareUpdateEvent& event) {
  if (const auto bytes = fw::encodeEvent(event)) (void)plugin::sendEvent(fw::kFacetName, *bytes);
}

}

void UpdateGate::close(std::string reason) {
  std::lock_guard lock(mutex_);
  reason_ = std::move(reason);
}

std::optional<std::string> UpdateGate::closedReason() const {
  std::lock_guard lock(mutex_);
  return reason_;
}

GatedDevice::GatedDevice(std::shared_ptr<SampleStreamDevice> device, std::shared_ptr<UpdateGate> gate)
    : device_(std::move(device)), gate_(std::move(gate)) {}

std::expected<void, std::string> GatedDevice::sendFramed(FramedCommand cmd, std::span<const uint8_t> payload) {
  if (auto reason = gate_->closedReason()) return std::unexpected(std::move(*reason));
  return device_->sendFramed(cmd, payload);
}

void GatedDevice::streamSamples(bitoscope::plugin::Sink& sink, const bitoscope::plugin::UsbStream& stream) {
  if (auto reason = gate_->closedReason()) {
    sink.fail(*reason);
    return;
  }
  device_->streamSamples(sink, stream);
}

bitoscope::plugin::FacetInfo firmwareUpdateFacetInfo(bool in_bootloader, std::string_view device_name) {
  fw::FirmwareUpdateInfo info;
  info.running = in_bootloader ? fw::Running::bootloader : fw::Running::program;
  info.model = std::string(device_name);
  info.modelChoices = {std::string(kModel16), std::string(kModel32)};
  info.fileDescription = "FNIRSI DLA firmware file (.bin)";
  info.imageNames = {"MCU", "FPGA"};
  info.estimatedSeconds = kEstimatedSeconds;
  const auto encoded = fw::encodeInfo(info);
  return plugin::FacetInfo{std::string(fw::kFacetName), 1, 0, {}, encoded ? *encoded : std::vector<uint8_t>{}};
}

DlaFirmwareUpdate::DlaFirmwareUpdate(FirmwareUpdateSetup setup, std::shared_ptr<UpdateGate> gate)
    : setup_(std::move(setup)), gate_(std::move(gate)) {}

DlaFirmwareUpdate::~DlaFirmwareUpdate() {
  FirmwareFlasher* flasher = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (running_) flasher = flasher_.get();
  }
  if (flasher != nullptr) (void)flasher->requestCancel(true);
  if (worker_.joinable()) worker_.join();
}

std::expected<plugin::CommandAnswer, std::string> DlaFirmwareUpdate::command(std::span<const uint8_t> payload) {
  const auto decoded = fw::decodeCommand(payload);
  if (!decoded) return std::unexpected(decoded.error().message);
  return std::visit(
      [this](const auto& c) -> Answer {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, fw::BeginUpload>) {
          return beginUpload(c);
        } else if constexpr (std::is_same_v<T, fw::UploadPiece>) {
          return uploadPiece(c);
        } else if constexpr (std::is_same_v<T, fw::Check>) {
          return check(c);
        } else if constexpr (std::is_same_v<T, fw::Start>) {
          return start(c);
        } else if constexpr (std::is_same_v<T, fw::Cancel>) {
          return cancel(c);
        } else if constexpr (std::is_same_v<T, fw::StartProgram>) {
          return startProgram();
        } else {
          return status();
        }
      },
      *decoded);
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::beginUpload(const fw::BeginUpload& begin) {
  std::lock_guard lock(mutex_);
  if (running_) return refused(std::string(kRunningReason));
  if (begin.totalBytes > kUploadLimit) {
    return refused(std::format("file of {} bytes is larger than {} bytes any firmware file has",
                               withThousands(begin.totalBytes), withThousands(kUploadLimit)));
  }
  ++upload_id_;
  upload_total_ = begin.totalBytes;
  upload_name_ = begin.fileName;
  upload_.clear();
  upload_.reserve(static_cast<size_t>(begin.totalBytes));
  return answered(std::format("upload {} started: {} bytes expected", upload_id_, withThousands(upload_total_)),
                  fw::encodeUploadReply(fw::UploadReply{upload_id_}));
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::uploadPiece(const fw::UploadPiece& piece) {
  std::lock_guard lock(mutex_);
  if (running_) return refused(std::string(kRunningReason));
  if (piece.uploadId != upload_id_ || upload_total_ == 0) {
    return refused(std::format("upload {} is not current upload", piece.uploadId));
  }
  if (piece.offset != upload_.size()) {
    return refused(std::format("piece starts at byte {}, upload continues at byte {}", withThousands(piece.offset),
                               withThousands(upload_.size())));
  }
  if (upload_.size() + piece.bytes.size() > upload_total_) {
    return refused(std::format("piece runs past announced {} bytes", withThousands(upload_total_)));
  }
  upload_.insert(upload_.end(), piece.bytes.begin(), piece.bytes.end());
  return plugin::CommandAnswer{
      true, std::format("{} of {} bytes received", withThousands(upload_.size()), withThousands(upload_total_)), {}};
}

std::optional<std::string> DlaFirmwareUpdate::uploadIncomplete(uint32_t upload_id) const {
  if (upload_id != upload_id_ || upload_total_ == 0) return std::format("upload {} is not current upload", upload_id);
  if (upload_.size() != upload_total_) {
    return std::format("upload is not complete: {} of {} bytes", withThousands(upload_.size()),
                       withThousands(upload_total_));
  }
  return std::nullopt;
}

DlaFirmwareUpdate::Verdict DlaFirmwareUpdate::judge(std::string_view named_model, FlashConfirmations confirmed) const {
  Verdict verdict;
  auto file = parseFirmwareFile(upload_);
  if (!file) {
    verdict.reply = fw::CheckReply{fw::Verdict::unreadable, {}, {}, file.error()};
    return verdict;
  }
  verdict.reply.sha256.assign(file->sha256.begin(), file->sha256.end());
  const std::optional<FirmwareModel> fileModel =
      file->known_model ? file->known_model : firmwareModelHintOf(upload_name_);
  if (fileModel) verdict.reply.fileModel = modelText(*fileModel);
  const std::optional<FirmwareModel> analyzer =
      firmwareModelOfDevice(setup_.in_bootloader ? named_model : std::string_view(setup_.device_name));
  const auto fit = checkFirmwareFitsAnalyzer(*file, upload_name_, analyzer, confirmed);
  if (!fit) {
    verdict.reply.verdict = verdictOf(fit.error().kind);
    verdict.reply.message = fit.error().message;
    return verdict;
  }
  verdict.reply.verdict = fw::Verdict::fits;
  verdict.reply.message = std::format("This firmware file suits your {}-channel analyzer. You can start update.",
                                      *analyzer == FirmwareModel::k16Channels ? 16 : 32);
  verdict.file = std::move(*file);
  return verdict;
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::check(const fw::Check& check) {
  std::lock_guard lock(mutex_);
  if (auto incomplete = uploadIncomplete(check.uploadId)) return refused(std::move(*incomplete));
  const Verdict verdict = judge(check.model, FlashConfirmations{check.confirmUnknownFile, check.confirmOtherModel});
  return answered(verdict.reply.message, fw::encodeCheckReply(verdict.reply));
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::start(const fw::Start& start) {
  joinFinishedWorker();
  std::lock_guard lock(mutex_);
  if (running_) return refused(std::string(kRunningReason));
  if (!setup_.make_transport && !transport_) return refused(std::string(kNoTransport));
  if (auto incomplete = uploadIncomplete(start.uploadId)) return refused(std::move(*incomplete));
  const Verdict verdict = judge(start.model, FlashConfirmations{start.confirmUnknownFile, start.confirmOtherModel});
  if (!verdict.file) {
    const auto payload = fw::encodeCheckReply(verdict.reply);
    return plugin::CommandAnswer{false, verdict.reply.message, payload ? *payload : std::vector<uint8_t>{}};
  }
  const FirmwareFile& file = *verdict.file;
  launch(FlashImages{file.mcu.wire_name, imageBytes(upload_, file.mcu), file.fpga.wire_name,
                     imageBytes(upload_, file.fpga)},
         false);
  return plugin::CommandAnswer{true, "firmware update started", {}};
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::cancel(const fw::Cancel& cancel) {
  FirmwareFlasher* flasher = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (running_) flasher = flasher_.get();
  }
  fw::CancelAnswer answer = fw::CancelAnswer::tooLate;
  std::string message = "no firmware update runs";
  if (flasher != nullptr) {
    switch (flasher->requestCancel(cancel.confirmCannotCapture)) {
      case CancelAnswer::kAccepted:
        answer = fw::CancelAnswer::accepted;
        message = "cancel accepted: update stops at next safe point";
        break;
      case CancelAnswer::kNeedsConfirmation:
        answer = fw::CancelAnswer::needsConfirmation;
        message = "update is inside FPGA image: cancel needs confirmation that analyzer cannot capture until update "
                  "completes";
        break;
      case CancelAnswer::kTooLate:
        message = "update is finishing: too late to cancel";
        break;
    }
  }
  return answered(std::move(message), fw::encodeCancelReply(fw::CancelReply{answer}));
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::startProgram() {
  joinFinishedWorker();
  std::lock_guard lock(mutex_);
  if (running_) return refused(std::string(kRunningReason));
  if (!setup_.make_transport && !transport_) return refused(std::string(kNoTransport));
  launch(FlashImages{}, true);
  return plugin::CommandAnswer{true, "analyzer starts its program", {}};
}

DlaFirmwareUpdate::Answer DlaFirmwareUpdate::status() {
  std::lock_guard lock(mutex_);
  return answered(running_ ? "firmware update runs" : "no firmware update runs",
                  fw::encodeStatusReply(fw::StatusReply{running_, last_}));
}

void DlaFirmwareUpdate::launch(FlashImages images, bool program_only) {
  if (!transport_) transport_ = setup_.make_transport();
  gate_->close(std::string(kRunningReason));
  running_ = true;
  last_.reset();
  last_phase_ = FlashPhase::kPreparing;
  last_attempt_ = 0;
  last_progress_sent_ = {};
  FlashEvents events;
  events.on_progress = [this](const FlashProgress& progress) { onProgress(progress); };
  flasher_ = std::make_unique<FirmwareFlasher>(*transport_, std::move(images), std::move(events), setup_.tuning.timing,
                                               FlashLimits{}, setup_.tuning.sleep);
  FirmwareFlasher* flasher = flasher_.get();
  worker_ = std::jthread([this, flasher, program_only] {
    const FlashResult result = program_only ? flasher->startProgram() : flasher->run();
    onFinished(result);
  });
}

void DlaFirmwareUpdate::joinFinishedWorker() {
  {
    std::lock_guard lock(mutex_);
    if (running_) return;
  }
  if (worker_.joinable()) worker_.join();
}

void DlaFirmwareUpdate::onProgress(const FlashProgress& progress) {
  const auto now = std::chrono::steady_clock::now();
  fw::FirmwareUpdateEvent event =
      fw::Progress{phaseOf(progress.phase), imageNameOf(progress.phase), progress.bytes_sent, progress.bytes_total,
                   static_cast<uint32_t>(progress.attempt), static_cast<uint32_t>(FlashLimits{}.attempts),
                   static_cast<uint32_t>(progress.resends)};
  bool send = false;
  {
    std::lock_guard lock(mutex_);
    send = progress.phase != last_phase_ || progress.attempt != last_attempt_ ||
           now - last_progress_sent_ >= setup_.tuning.progress_interval || progress.bytes_sent == progress.bytes_total;
    last_ = event;
    if (send) {
      last_phase_ = progress.phase;
      last_attempt_ = progress.attempt;
      last_progress_sent_ = now;
    }
  }
  if (send) publish(event);
}

void DlaFirmwareUpdate::onFinished(const FlashResult& result) {
  const fw::FirmwareUpdateEvent event = fw::Finished{outcomeOf(result.outcome), result.message, result.can_capture,
                                                     static_cast<uint32_t>(result.attempts)};
  gate_->close(std::string(kRanReason));
  {
    std::lock_guard lock(mutex_);
    running_ = false;
    last_ = event;
  }
  publish(event);
}

}

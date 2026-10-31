/*
 * SPDX-License-Identifier: MIT
 */

#include "signal_generator_facet.h"

#include <exception>
#include <string>
#include <utility>
#include <variant>

#include "generic_facet.h"
#include "ipc_decode_error.h"
#include "ipc_signal_generator.h"

namespace bitoscope::plugin_c_sdk {

namespace {

namespace siggen = ipc::siggen;

class SignalGeneratorFacet final : public FacetHandler {
 public:
  explicit SignalGeneratorFacet(SignalGeneratorRegistration registration) : registration_(registration) {}

  FacetAnswer handle(std::span<const uint8_t> payload) override {
    auto command = siggen::decodeCommand(payload);
    if (!command) {
      const ipc::DecodeError& error = command.error();
      if (error.kind == ipc::DecodeErrorKind::unknownMember) {
        return ipc::Refusal{"facet \"" + std::string(siggen::kFacetName) +
                                "\" does not know this command: this driver's siggen does not know it: " + error.message,
                            0};
      }
      return ipc::FacetReply{false, "malformed SignalGeneratorCommand payload: " + error.message, {}};
    }
    bitoscope_reply reply;
    int status = BITOSCOPE_ERROR;
    std::string what = "stop";
    try {
      if (const auto* start = std::get_if<siggen::StartOutput>(&command->action)) {
        what = "start";
        status = output(registration_.start, command->channelMask, start->waveform, reply);
      } else if (const auto* configure = std::get_if<siggen::ConfigureOutput>(&command->action)) {
        what = "configure";
        status = output(registration_.configure, command->channelMask, configure->waveform, reply);
      } else {
        status = registration_.stop(registration_.session, command->channelMask, &reply);
      }
    } catch (const std::exception& e) {
      reply.message = "signal generator's " + what + " function threw: " + e.what();
    } catch (...) {
      reply.message = "signal generator's " + what + " function threw";
    }
    if (status != BITOSCOPE_OK && reply.message.empty()) {
      reply.message = "signal generator's " + what + " function failed (status " + std::to_string(status) + ")";
    }
    return ipc::FacetReply{status == BITOSCOPE_OK, std::move(reply.message), std::move(reply.payload)};
  }

  std::expected<void, ApplyRefusal> apply(std::span<const ipc::props::PropertyValue> values, bool capturing) override {
    return applyThrough(registration_.applyProperties, registration_.session, values, capturing);
  }

 private:
  int output(bitoscope_generator_output_fn function, uint32_t mask, const siggen::Waveform& waveform,
             bitoscope_reply& reply) const {
    const bitoscope_waveform given{waveform.name.c_str(), waveform.params.empty() ? nullptr : waveform.params.data(),
                                   waveform.params.size()};
    return function(registration_.session, mask, &given, &reply);
  }

  SignalGeneratorRegistration registration_;
};

}

std::unique_ptr<FacetHandler> makeSignalGeneratorFacet(SignalGeneratorRegistration registration) {
  return std::make_unique<SignalGeneratorFacet>(registration);
}

}

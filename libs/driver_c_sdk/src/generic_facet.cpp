/*
 * SPDX-License-Identifier: MIT
 */

#include "generic_facet.h"

#include <exception>
#include <utility>
#include <vector>

#include "facet_properties.h"

namespace bitoscope::plugin_c_sdk {

namespace {

class GenericFacet final : public FacetHandler {
 public:
  GenericFacet(std::string name, GenericRegistration registration)
      : name_(std::move(name)), registration_(registration) {}

  FacetAnswer handle(std::span<const uint8_t> payload) override {
    bitoscope_reply reply;
    int status = BITOSCOPE_ERROR;
    try {
      status = registration_.command(registration_.session, payload.data(), payload.size(), &reply);
    } catch (const std::exception& e) {
      reply.message = std::string("facet's command function threw: ") + e.what();
    } catch (...) {
      reply.message = "facet's command function threw";
    }
    if (status == BITOSCOPE_UNKNOWN_COMMAND) {
      return ipc::Refusal{"facet \"" + name_ + "\" does not know this command" +
                              (reply.message.empty() ? std::string() : ": " + reply.message),
                          0};
    }
    if (status != BITOSCOPE_OK && reply.message.empty()) {
      reply.message = "facet \"" + name_ + "\"'s command failed (status " + std::to_string(status) + ")";
    }
    return ipc::FacetReply{status == BITOSCOPE_OK, std::move(reply.message), std::move(reply.payload)};
  }

  std::expected<void, ApplyRefusal> apply(std::span<const ipc::props::PropertyValue> values, bool capturing) override {
    return applyThrough(registration_.applyProperties, registration_.session, values, capturing);
  }

 private:
  std::string name_;
  GenericRegistration registration_;
};

}

std::unique_ptr<FacetHandler> makeGenericFacet(std::string name, GenericRegistration registration) {
  return std::make_unique<GenericFacet>(std::move(name), registration);
}

namespace {

template <typename Apply>
std::expected<void, ApplyRefusal> appliedOrRefused(std::span<const ipc::props::PropertyValue> values, Apply&& apply) {
  const std::vector<bitoscope_property_value> given = cValues(values);
  bitoscope_reply reply;
  int status = BITOSCOPE_ERROR;
  try {
    status = apply(given, reply);
  } catch (const std::exception& e) {
    reply.message = std::string("apply function threw: ") + e.what();
  } catch (...) {
    reply.message = "apply function threw";
  }
  if (status == BITOSCOPE_OK) return {};
  return std::unexpected(ApplyRefusal{std::move(reply.message), std::move(reply.refusedProperty)});
}

}

std::expected<void, ApplyRefusal> applyThrough(bitoscope_apply_properties_fn apply, void* user,
                                               std::span<const ipc::props::PropertyValue> values, bool capturing) {
  if (apply == nullptr) return {};
  return appliedOrRefused(values, [&](const std::vector<bitoscope_property_value>& given, bitoscope_reply& reply) {
    return apply(user, given.data(), given.size(), capturing ? 1 : 0, &reply);
  });
}

std::expected<void, ApplyRefusal> applyThrough(bitoscope_apply_during_capture_fn apply, void* user,
                                               std::span<const ipc::props::PropertyValue> values) {
  if (apply == nullptr) return {};
  return appliedOrRefused(values, [&](const std::vector<bitoscope_property_value>& given, bitoscope_reply& reply) {
    return apply(user, given.data(), given.size(), &reply);
  });
}

}

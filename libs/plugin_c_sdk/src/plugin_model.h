/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "ipc_messages.h"

namespace bitoscope::plugin_c_sdk {

struct UsbId {
  uint16_t vendor = 0;
  uint16_t product = 0;
  bool operator==(const UsbId&) const = default;
};

using FacetAnswer = std::variant<ipc::FacetReply, ipc::Refusal>;

struct ApplyRefusal {
  std::string message;
  std::string propertyId;
};

class FacetHandler {
 public:
  virtual ~FacetHandler() = default;

  virtual FacetAnswer handle(std::span<const uint8_t> payload) = 0;

  virtual std::expected<void, ApplyRefusal> apply([[maybe_unused]] std::span<const ipc::props::PropertyValue> values,
                                                  [[maybe_unused]] bool capturing) {
    return {};
  }

  virtual bool capturing() const { return false; }

  virtual void abandon() {}
};

using FacetHandlerFactory =
    std::function<std::unique_ptr<FacetHandler>(const std::vector<ipc::props::PropertyValue>& values)>;

struct FacetPropertyTree {
  std::string facet;
  std::vector<ipc::props::PropertyNode> tree;
};

struct FacetRegistration {
  std::string name;
  uint32_t major = 1;
  uint32_t minor = 0;
  uint32_t protocolMajor = 0;
  std::vector<std::string> capabilities;
  ipc::Bytes info;
  FacetHandlerFactory makeHandler;
};

}

struct bitoscope_device {
  void* session = nullptr;
  bool facetsFixed = false;
  std::string id;
  std::string name;
  std::string serialNumber;
  std::vector<bitoscope::plugin_c_sdk::FacetRegistration> facets;
  std::vector<bitoscope::plugin_c_sdk::FacetPropertyTree> properties;

  const std::vector<bitoscope::ipc::props::PropertyNode>& propertiesOf(const std::string& facet) const {
    static const std::vector<bitoscope::ipc::props::PropertyNode> kNone;
    for (const auto& p : properties) {
      if (p.facet == facet) return p.tree;
    }
    return kNone;
  }
};

struct bitoscope_devices {
  std::vector<std::unique_ptr<bitoscope_device>> devices;
  std::string pluginVersion;
  std::string listProblem;
};

struct bitoscope_reply {
  std::string message;
  bitoscope::ipc::Bytes payload;
  std::string refusedProperty;
};

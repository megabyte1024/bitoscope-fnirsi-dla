/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipc_properties.h"

namespace bitoscope::host {

inline constexpr std::string_view kTransportErrorPrefix = "transport error: ";

struct PropertiesRefusal {
  std::string message;
  std::string property;
};

class FacetTransport {
 public:
  virtual ~FacetTransport() = default;

  virtual std::expected<std::string, std::string> sendCommand(std::string_view plugin_id,
                                                               std::string_view facet,
                                                               std::string_view payload) = 0;

  // WHY: setProperties() is not pure virtual: most transports predate properties; the base answers as an old driver.
  virtual std::expected<void, PropertiesRefusal> setProperties(std::string_view plugin_id, std::string_view facet,
                                                                std::span<const ipc::props::PropertyValue> values) {
    (void)plugin_id;
    (void)facet;
    (void)values;
    return std::unexpected(
        PropertiesRefusal{std::string(kTransportErrorPrefix) + "this transport does not implement setProperties", {}});
  }
};

class FacetClient {
 public:
  FacetClient(FacetTransport& transport, std::string plugin_id, std::string facet)
      : transport_(&transport),
        plugin_id_(std::move(plugin_id)),
        facet_(std::move(facet)) {}

  std::expected<std::string, std::string> call(std::string_view payload) const {
    return transport_->sendCommand(plugin_id_, facet_, payload);
  }

  std::expected<void, PropertiesRefusal> setProperties(std::span<const ipc::props::PropertyValue> values) const {
    return transport_->setProperties(plugin_id_, facet_, values);
  }

  std::string_view pluginId() const { return plugin_id_; }
  std::string_view facet() const { return facet_; }

 private:
  FacetTransport* transport_;
  std::string plugin_id_;
  std::string facet_;
};

}

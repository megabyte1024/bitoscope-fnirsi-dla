/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "bitoscope_driver.h"
#include "driver_model.h"
#include "plugin_model.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

std::unique_ptr<FacetHandler> makeGenericFacet(std::string name, GenericRegistration registration);

using OwnFacetMaker = std::function<std::unique_ptr<FacetHandler>(const std::string& name, void* session)>;

int addOwnFacet(bitoscope_device* device, const bitoscope_facet& facet, bool commandRequired, OwnFacetMaker maker,
                bitoscope_reply* reply);

std::expected<void, ApplyRefusal> applyThrough(bitoscope_apply_properties_fn apply, void* user,
                                               std::span<const ipc::props::PropertyValue> values, bool capturing);

std::expected<void, ApplyRefusal> applyThrough(bitoscope_apply_during_capture_fn apply, void* user,
                                               std::span<const ipc::props::PropertyValue> values);

}

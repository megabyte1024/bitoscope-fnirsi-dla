/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string>

#include "bitoscope_driver.h"
#include "driver_model.h"
#include "plugin_model.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

std::unique_ptr<FacetHandler> makeGenericFacet(std::string name, GenericRegistration registration);

std::expected<void, ApplyRefusal> applyThrough(bitoscope_apply_properties_fn apply, void* user,
                                               std::span<const ipc::props::PropertyValue> values, bool capturing);

}

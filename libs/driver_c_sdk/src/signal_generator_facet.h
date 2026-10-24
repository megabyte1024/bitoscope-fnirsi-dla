/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>

#include "driver_model.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

std::unique_ptr<FacetHandler> makeSignalGeneratorFacet(SignalGeneratorRegistration registration);

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>
#include <vector>

#include "ipc_decode_error.h"
#include "ipc_properties.h"
#include "properties.capnp.h"

namespace bitoscope::ipc::props::detail {

void build(capnp::List<wire::props::PropertyNode>::Builder out, const std::vector<PropertyNode>& in);
void build(wire::props::SetProperties::Builder out, const SetProperties& in);

std::optional<DecodeError> read(capnp::List<wire::props::PropertyNode>::Reader in, std::vector<PropertyNode>& out);
std::optional<DecodeError> read(wire::props::SetProperties::Reader in, SetProperties& out);

}

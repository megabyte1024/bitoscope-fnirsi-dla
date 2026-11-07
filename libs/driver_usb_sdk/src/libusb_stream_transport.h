/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include "usb_stream_transport.h"

namespace bitoscope::plugin_c_sdk {

TransferStatus transferStatusOf(int libusbTransferStatus);

UsbError usbErrorOf(int libusbError);

std::expected<uint64_t, UsbError> bulkReadAnswer(int libusbResult, int transferred);

std::unique_ptr<UsbStreamTransport> makeLibusbStreamTransport(void* context, void* handle, uint8_t endpoint);

}

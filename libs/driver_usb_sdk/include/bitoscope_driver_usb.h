/*
 * SPDX-License-Identifier: MIT
 */

#ifndef BITOSCOPE_DRIVER_USB_H
#define BITOSCOPE_DRIVER_USB_H

#include "bitoscope_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bitoscope_usb_location {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t interface_number; /* IMPLEMENT: the interface your bitoscope_usb_id entry claims */
  uint8_t bus;
  uint8_t address;      /* IMPLEMENT: changes each time the device enumerates */
  const uint8_t* ports; /* IMPLEMENT: the port path from the root hub: 4, 2 for id "usb-1-4.2" */
  size_t num_ports;
} bitoscope_usb_location;

/* IMPLEMENT: One USB product your driver drives: the SDK finds these devices itself. */
typedef struct bitoscope_usb_id {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t interface_number; /* IMPLEMENT: the interface your driver claims */
  const char* generic_name; /* IMPLEMENT: optional: names a busy device never identified; NULL: display_name */
} bitoscope_usb_id;

bitoscope_finder* bitoscope_usb_finder(const bitoscope_usb_id* ids, size_t num_ids);

int bitoscope_usb_location_of(const bitoscope_found_device* device, bitoscope_usb_location* location);

int bitoscope_add_usb_device(bitoscope_found_devices* found, const char* id, const bitoscope_usb_location* location,
                             const char* appearance_key);

#define BITOSCOPE_USB_TRANSFERS_PROPERTY "usb_transfers"
#define BITOSCOPE_USB_TRANSFER_KIB_PROPERTY "usb_transfer_kib"

const bitoscope_property* bitoscope_usb_stream_properties(size_t* count);

typedef struct bitoscope_usb_device bitoscope_usb_device;

#define BITOSCOPE_DEVICE_GONE (-130)
#define BITOSCOPE_USB_TIMEOUT (-131)

#define BITOSCOPE_USB_SPEED_UNKNOWN 0
#define BITOSCOPE_USB_SPEED_LOW 1
#define BITOSCOPE_USB_SPEED_FULL 2
#define BITOSCOPE_USB_SPEED_HIGH 3
#define BITOSCOPE_USB_SPEED_SUPER 4
#define BITOSCOPE_USB_SPEED_SUPER_PLUS 5

int bitoscope_usb_open(const bitoscope_found_device* device, bitoscope_usb_device** usb, bitoscope_reply* reply);

void bitoscope_usb_close(bitoscope_usb_device* usb);

int bitoscope_usb_speed(const bitoscope_usb_device* usb);

int bitoscope_usb_bulk_out(bitoscope_usb_device* usb, uint8_t endpoint, const void* data, size_t size,
                           uint32_t timeout_ms, size_t* transferred);

int bitoscope_usb_bulk_in(bitoscope_usb_device* usb, uint8_t endpoint, void* data, size_t size, uint32_t timeout_ms,
                          size_t* transferred);

int bitoscope_usb_control(bitoscope_usb_device* usb, uint8_t request_type, uint8_t request, uint16_t value,
                          uint16_t index, void* data, uint16_t size, uint32_t timeout_ms, size_t* transferred);

int bitoscope_usb_clear_halt(bitoscope_usb_device* usb, uint8_t endpoint);

const char* bitoscope_usb_error_text(void);

void* bitoscope_usb_libusb_context(bitoscope_usb_device* usb);

void* bitoscope_usb_libusb_handle(bitoscope_usb_device* usb);

/* IMPLEMENT: Optional: called once, on the capture thread, when every transfer is in flight: send your device's
 * start command here. BITOSCOPE_OK streams on; anything else fails the capture (say why with
 * bitoscope_fail_capture() first). */
typedef int (*bitoscope_usb_started_fn)(void* user);

/* IMPLEMENT: Optional: called once, on the capture thread, when the stream has ended and no transfer is in flight
 * any more, unless your device is gone: send your device's stop command here. Anything but BITOSCOPE_OK is logged
 * as a warning; the capture's outcome stays as it is. */
typedef int (*bitoscope_usb_stop_fn)(void* user);

/* IMPLEMENT: Optional: turns `num_units` whole units of your device's bytes (unit_bytes each, back to back) into
 * num_units * unit_samples samples of your capture's device bytes (bitoscope_device_bytes),
 * bitoscope_sample_bytes(sink, unit_samples) bytes a unit, at `samples`. BITOSCOPE_OK goes on; anything else fails
 * the capture (bitoscope_fail_capture() first). */
typedef int (*bitoscope_usb_convert_fn)(void* user, const uint8_t* device_bytes, uint64_t num_units, uint8_t* samples);

/* IMPLEMENT: How bitoscope_stream_usb() streams your device's bulk IN endpoint into the sink. Fill it in (start
 * from all zeros); it is read during the call only. */
typedef struct bitoscope_usb_stream {
  bitoscope_usb_device* device; /* IMPLEMENT: required: your device, as bitoscope_usb_open() gave it */
  uint8_t endpoint;              /* IMPLEMENT: required: the bulk IN endpoint your device sends samples on, 0x81 say */
  /* IMPLEMENT: Optional from here on: leave them 0. */
  void* user;                       /* IMPLEMENT: passed to started, convert and stop */
  bitoscope_usb_started_fn started; /* IMPLEMENT: NULL: you started your device before the call */
  /* IMPLEMENT: 1: what the endpoint holds is read and thrown away before any transfer is queued; bytes still
   * arriving after about 1 s of that fail the capture at sample 0, and the host retries it as start-up loss. Use
   * it only when your device starts in `started`: the bytes of a device already started would go too. */
  int drain;
  /* IMPLEMENT: Samples thrown away at the start of the stream (a settling time, say), rounded up to whole
   * units: unit_samples with convert, else 8 samples in BITOSCOPE_CHANNEL_MAJOR and 1 in BITOSCOPE_SAMPLE_MAJOR. */
  uint64_t preroll_samples;
  /* IMPLEMENT: Bytes of each transfer, a multiple of 1024 up to 16 MiB. 0: about 0.1 s of your device's data,
   * rounded up to whole KiB, at most 1 MiB, so the first data arrives within about 0.2 s at any rate. Leave this and
   * transfers 0 when your logic analyzer lists bitoscope_usb_stream_properties() after its own properties (in
   * "Diagnostics"): the user then sets both, and a size of 0 is the SDK's own sizing. */
  uint32_t transfer_bytes;
  uint32_t transfers; /* IMPLEMENT: transfers kept in flight, 2 to 64; 0: 8 */
  /* IMPLEMENT: Delivery that falls this many of your device's bytes behind its sample clock fails the capture:
   * about what your device can hold before it drops data. 0: not checked; never in a buffered capture. */
  uint64_t lag_limit_bytes;
  /* IMPLEMENT: For a device whose bytes no bitoscope_device_bytes describes: convert turns each unit of unit_bytes
   * device bytes into unit_samples samples (a multiple of 8 in BITOSCOPE_CHANNEL_MAJOR). NULL and 0: your device
   * sends the capture's device bytes. */
  bitoscope_usb_convert_fn convert;
  uint32_t unit_bytes;
  uint32_t unit_samples;
  const char* device_name; /* IMPLEMENT: names your device in messages ("DLA was disconnected"); NULL: "USB device" */
  bitoscope_usb_stop_fn stop; /* IMPLEMENT: NULL: nothing is called */
} bitoscope_usb_stream;

#define bitoscope_stream_usb(sink, stream) bitoscope_stream_usb_sized((sink), (stream), sizeof(*(stream)))

void bitoscope_stream_usb_sized(bitoscope_sink* sink, const bitoscope_usb_stream* stream, size_t stream_size);

#ifdef __cplusplus
}
#endif

#endif

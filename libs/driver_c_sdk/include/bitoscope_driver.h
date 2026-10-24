/*
 * SPDX-License-Identifier: MIT
 */

// WHY: Use a sink from ONE thread at a time, the capture thread or one it joins; bitoscope_stopping() from any.
#ifndef BITOSCOPE_DRIVER_H
#define BITOSCOPE_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "bitoscope_plugin_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IMPLEMENT: Returned by a facet's command function: "I do not know this command". The host gets a refusal. */
#define BITOSCOPE_UNKNOWN_COMMAND (-110)

typedef struct bitoscope_device bitoscope_device;

int bitoscope_set_name(bitoscope_device* device, const char* name);

int bitoscope_set_serial_number(bitoscope_device* device, const char* serial_number);

/* IMPLEMENT: What an identify or open function returns when it cannot use the device now. The host shows the same
 * words for every driver and knows when waiting helps. Any other value but BITOSCOPE_OK (BITOSCOPE_ERROR, say)
 * is "other": give its text with bitoscope_reply_message(). */
#define BITOSCOPE_BUSY (-100)             /* IMPLEMENT: another program holds the device; the SDK tries once more */
#define BITOSCOPE_NO_PERMISSION (-101)    /* IMPLEMENT: the system denies your driver access to the device */
#define BITOSCOPE_FIRMWARE_LOADING (-102) /* IMPLEMENT: the device loads its firmware and will enumerate again; SDK waits */
#define BITOSCOPE_NOT_SUPPORTED (-103)    /* IMPLEMENT: your driver knows the device but does not serve it */

/* IMPLEMENT: What an open function reports while it works (bitoscope_report_progress()). */
#define BITOSCOPE_PROGRESS_OTHER 0              /* IMPLEMENT: say what in the detail */
#define BITOSCOPE_PROGRESS_FIRMWARE_LOADING 1   /* IMPLEMENT: the device's firmware is being loaded */
#define BITOSCOPE_PROGRESS_WAITING_FOR_DEVICE 2 /* IMPLEMENT: waiting for the device to enumerate again */

typedef struct bitoscope_progress bitoscope_progress;
typedef struct bitoscope_found_devices bitoscope_found_devices;

/* IMPLEMENT: The transport a device connects through: what your driver description declares (`transport`), and
 * what each device was found on (bitoscope_found_device's `transport`). One value per transport, the same
 * everywhere; a new transport is a new value.
 *   BITOSCOPE_TRANSPORT_NONE     no connection at all (a simulation); a found device: no place given.
 *   BITOSCOPE_TRANSPORT_USB      USB: give usb_ids; the SDK finds the devices and needs no connection.
 *   BITOSCOPE_TRANSPORT_SERIAL   a serial port: the host gives its name (bitoscope_connection's serial_port).
 *   BITOSCOPE_TRANSPORT_NETWORK  a network address and usually a port (bitoscope_connection's network_). */
typedef enum bitoscope_transport {
  BITOSCOPE_TRANSPORT_NONE = 0,
  BITOSCOPE_TRANSPORT_USB = 1,
  BITOSCOPE_TRANSPORT_SERIAL = 2,
  BITOSCOPE_TRANSPORT_NETWORK = 3
} bitoscope_transport;

/* IMPLEMENT: The connection the host gave a SERIAL or NETWORK driver (--serial-port, --network-address,
 * --network-port): NULL and 0 where none was given. */
typedef struct bitoscope_connection {
  const char* network_address;
  uint16_t network_port;
  const char* serial_port;
} bitoscope_connection;

typedef struct bitoscope_usb_location {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t interface_number; /* IMPLEMENT: the interface your usb_ids entry claims */
  uint8_t bus;
  uint8_t address;      /* IMPLEMENT: changes each time the device enumerates */
  const uint8_t* ports; /* IMPLEMENT: the port path from the root hub: 4, 2 for id "usb-1-4.2" */
  size_t num_ports;
} bitoscope_usb_location;

typedef struct bitoscope_serial_location {
  const char* port; /* IMPLEMENT: the name you open the port by: "/dev/ttyACM0", "COM3" */
} bitoscope_serial_location;

typedef struct bitoscope_network_location {
  const char* address; /* IMPLEMENT: a host name or an IP address */
  uint16_t port;       /* IMPLEMENT: 0: none */
} bitoscope_network_location;

/* IMPLEMENT: The device an identify, open or held check is about, as the SDK found it, valid during the
 * call. Your enumerate function fills one in (start from all zeros) for bitoscope_add_located_device(); the SDK
 * copies it, and gives it back to the other functions as it was given. */
typedef struct bitoscope_found_device {
  const char* id;                         /* IMPLEMENT: the device's id: the user selects the device by it */
  const bitoscope_connection* connection; /* IMPLEMENT: never NULL; ignored when you add a device */
  /* IMPLEMENT: What the device was found on, and the member below that holds its place: a new transport gets
   * a member of its own at the end. NONE: no place given (your enumerate function added it without one). */
  bitoscope_transport transport;
  const bitoscope_usb_location* usb;         /* IMPLEMENT: NULL unless transport is BITOSCOPE_TRANSPORT_USB */
  const bitoscope_serial_location* serial;   /* IMPLEMENT: NULL unless transport is BITOSCOPE_TRANSPORT_SERIAL */
  const bitoscope_network_location* network; /* IMPLEMENT: NULL unless transport is BITOSCOPE_TRANSPORT_NETWORK */
  /* IMPLEMENT: Optional: changes each time the device appears anew (it enumerated again, restarted, was plugged
   * in again), and the SDK then identifies it again. NULL or "": it keeps its appearance while present. */
  const char* appearance_key;
} bitoscope_found_device;

/* IMPLEMENT: Required unless your driver gives usb_ids: adds every device present, by its id with
 * bitoscope_add_found_device(), or with its place and appearance key with bitoscope_add_located_device(), and
 * returns BITOSCOPE_OK (none at all is fine). Any other value: your driver could not look for devices. Identify
 * gives each device its name and facets. */
typedef int (*bitoscope_enumerate_fn)(void* user, const bitoscope_connection* connection,
                                      bitoscope_found_devices* found);

/* IMPLEMENT: Required: describes the device the SDK found (`found`) in `device`, which the SDK made with its id:
 * its name with bitoscope_set_name() (none: its generic name), optionally its serial number, and its facets with
 * bitoscope_add_logic_analyzer() and the rest; returns BITOSCOPE_OK. It may open the device briefly to ask it.
 * When it cannot, it returns BITOSCOPE_BUSY, BITOSCOPE_NO_PERMISSION, BITOSCOPE_FIRMWARE_LOADING or
 * BITOSCOPE_NOT_SUPPORTED, or another value with its text given by bitoscope_reply_message(reply, ...); what it
 * described is then dropped. The SDK calls it each time the device appears, and before every session. */
typedef int (*bitoscope_identify_fn)(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                                     bitoscope_reply* reply);

/* IMPLEMENT: Optional: opens the device for the session, after identify described it in `device`, and keeps it
 * until close. It may set `device`'s name and serial number again (read from the opened device); its facets are
 * identify's. `*session` starts as your driver's `user`: set it to what the functions of the device's facets
 * (capture, start, apply_properties, the generator's) get as their first argument, and close frees. Returns
 * BITOSCOPE_OK, or a value as identify does. While it works it may report progress with
 * bitoscope_report_progress(progress, ...) (valid during the call): the host shows each report and waits on.
 * NULL: those functions get your driver's `user`, and your capture opens the device. */
typedef int (*bitoscope_open_fn)(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                                 bitoscope_progress* progress, void** session, bitoscope_reply* reply);

/* IMPLEMENT: Optional, with open: releases what open kept, and frees `session`. Called once, when the session
 * ends, after an open that returned BITOSCOPE_OK, also when the host's handshake was refused. NULL: nothing is
 * called. */
typedef void (*bitoscope_close_fn)(void* user, void* session);

/* IMPLEMENT: Optional: says, without using the device, whether another program holds it: BITOSCOPE_BUSY for
 * held, BITOSCOPE_OK for free. It decides for every device of your driver. NULL: devices found by your usb_ids
 * get the SDK's own check; those your enumerate function adds count as free. The SDK calls neither identify nor
 * open for a device whose check finds it held or without permission. */
typedef int (*bitoscope_held_check_fn)(void* user, const bitoscope_found_device* device);

/* IMPLEMENT: One USB product your driver drives: the SDK finds these devices itself. */
typedef struct bitoscope_usb_id {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t interface_number; /* IMPLEMENT: the interface your driver claims */
  const char* generic_name; /* IMPLEMENT: optional: names a busy device never identified; NULL: display_name */
} bitoscope_usb_id;

/* IMPLEMENT: Your driver. Fill this in (start from all zeros) and pass it to bitoscope_run_driver() from main();
 * the SDK copies it, so it can live on the stack, and the arrays it points to must live while it runs. */
typedef struct bitoscope_driver {
  const char* display_name;                /* IMPLEMENT: required: your driver's name, as users see it */
  /* IMPLEMENT: Required: your driver's version as X.Y.Z with an optional suffix: "1.0.0", "1.2.3-beta",
   * "2.0.3-alpha01". Three numbers without leading zeros; then "-" and letters, digits and dots (at most 32
   * characters in all). */
  const char* version;
  bitoscope_transport transport; /* IMPLEMENT: required: BITOSCOPE_TRANSPORT_USB when you give usb_ids */
  /* IMPLEMENT: Required: how your devices are found, ONE of these: the USB products the SDK finds, or your own
   * enumerate function (for devices that are not USB). */
  const bitoscope_usb_id* usb_ids;
  size_t num_usb_ids;
  bitoscope_enumerate_fn enumerate;
  bitoscope_identify_fn identify; /* IMPLEMENT: required */
  /* IMPLEMENT: Optional from here on: leave them 0. */
  void* user; /* IMPLEMENT: passed to every function of this struct; without open, the session of every device */
  bitoscope_open_fn open;
  bitoscope_close_fn close;
  bitoscope_held_check_fn held_check;
} bitoscope_driver;

#define bitoscope_run_driver(argc, argv, driver) \
  bitoscope_run_driver_sized((argc), (argv), (driver), sizeof(*(driver)))

int bitoscope_run_driver_sized(int argc, char** argv, const bitoscope_driver* driver, size_t driver_size);

int bitoscope_add_found_device(bitoscope_found_devices* found, const char* id);

#define bitoscope_add_located_device(found, device) \
  bitoscope_add_located_device_sized((found), (device), sizeof(*(device)))

int bitoscope_add_located_device_sized(bitoscope_found_devices* found, const bitoscope_found_device* device,
                                       size_t device_size);

int bitoscope_report_progress(bitoscope_progress* progress, int code, const char* detail);

typedef struct bitoscope_sink bitoscope_sink;

/* IMPLEMENT: How your device's bytes hold its samples: the layout.
 *   BITOSCOPE_SAMPLE_MAJOR   (the default) one sample after another; in each, the channels your bytes carry in
 *                            mask order, the lowest at bit 0 of the first byte, the next at bit 1, and so on;
 *                            bits and bytes after them are not read.
 *   BITOSCOPE_CHANNEL_MAJOR  rounds of 8 samples; in each, one byte for each channel your bytes carry, in mask
 *                            order, holding 8 consecutive samples of that channel, the first in bit 0; bytes
 *                            after them are not read. */
#define BITOSCOPE_SAMPLE_MAJOR 0
#define BITOSCOPE_CHANNEL_MAJOR 1

/* IMPLEMENT: The bytes your device sends: write them as they come, and the SDK keeps the capture's channels, in
 * mask order (the lowest captured channel becomes channel 0). Start from all zeros: sample-major, every channel,
 * as few bytes as they take. */
typedef struct bitoscope_device_bytes {
  int layout; /* IMPLEMENT: BITOSCOPE_SAMPLE_MAJOR or BITOSCOPE_CHANNEL_MAJOR */
  /* IMPLEMENT: The channels the bytes carry: those of the capture's channel_mask and any others. 0: every channel
   * of your logic analyzer. */
  uint32_t channel_mask;
  /* IMPLEMENT: Bytes from one sample to the next (sample-major) or from one round to the next (channel-major).
   * 0: as few as the channels take, (n + 7) / 8 a sample or n a round for n channels. */
  uint32_t stride_bytes;
} bitoscope_device_bytes;

/* IMPLEMENT: One property's value (properties, below): what a capture and an apply function receive. Which of
 * the three value fields counts is the property's type; the other two are 0. This struct never grows:
 * the SDK hands out arrays of it. */
typedef struct bitoscope_property_value {
  const char* id;      /* IMPLEMENT: the property's id, as you gave it */
  int64_t integer;     // IMPLEMENT: BITOSCOPE_PROPERTY_INTEGER, and BITOSCOPE_PROPERTY_DECIMAL in fixed point: 1.60 with 2 digits is 160
  int boolean;         /* IMPLEMENT: BITOSCOPE_PROPERTY_BOOLEAN: 0 or 1 */
  uint32_t enum_index; /* IMPLEMENT: BITOSCOPE_PROPERTY_ENUMERATION: the index of the chosen item */
} bitoscope_property_value;

/* IMPLEMENT: What the host asked for. The SDK has checked it against your bitoscope_logic_analyzer: the mask
 * has no channel above your logic analyzer's num_channels - 1, the rate is one you listed, and it is not
 * above your rate ceiling for this many channels. */
typedef struct bitoscope_capture {
  uint64_t sample_rate_milli_hz; /* IMPLEMENT: 1 MHz is 1000000000 */
  uint32_t channel_mask;         /* IMPLEMENT: bit c set: channel c is captured */
  uint32_t num_captured_channels; /* IMPLEMENT: how many bits channel_mask has set: the channels in a sample */
  uint64_t sample_limit;         /* IMPLEMENT: how many samples to capture; 0: until the host says STOP */
  int buffered;                  // IMPLEMENT: 0: stream samples as they come. 1: device captures into its own memory; write it out at STOP or the limit
  /* IMPLEMENT: The current value of EVERY property of the logic analyzer (bitoscope_add_property()), in the
   * order the host lists them (depth first through the groups): a property the host never set has
   * its default. None: NULL and 0. Valid until the capture function returns; read one with
   * bitoscope_find_property_value(). */
  const bitoscope_property_value* properties;
  size_t num_properties;
} bitoscope_capture;

/* IMPLEMENT: Captures: writes samples until a write answers BITOSCOPE_CAPTURE_OVER, then returns. When the device
 * fails, it says why with bitoscope_fail_capture() (or bitoscope_device_gone() when it is unplugged) and returns:
 * that is the only way to fail a capture. Returning before a write answered BITOSCOPE_CAPTURE_OVER fails the
 * capture too, and the SDK says so. Before its first write it may describe this capture's bytes with
 * bitoscope_set_device_bytes(), when only the device's first data tells them. Runs on a capture thread of the
 * SDK's, one capture at a time. `session` is what open gave. */
typedef void (*bitoscope_capture_fn)(void* session, const bitoscope_capture* capture, bitoscope_sink* sink);

/* IMPLEMENT: Optional: prepares a START before the capture begins, on the SDK's main thread, after the SDK's own
 * checks of the mask, the rate and the ring. BITOSCOPE_OK starts the capture; anything else refuses the
 * START - the capture function is not called - with the reason you give with
 * bitoscope_reply_message(reply, ...). Here bitoscope_set_device_bytes() may describe this capture's bytes in
 * place of your analyzer's device_bytes; bitoscope_max_chunk_samples(), bitoscope_sample_bytes() and
 * bitoscope_ring_fill() work too. Writing samples does not: that is the capture function's. After it the
 * SDK refuses a START whose ring slot cannot hold one sample, or one round in BITOSCOPE_CHANNEL_MAJOR, of
 * the captured channels. */
typedef int (*bitoscope_prepare_fn)(void* session, const bitoscope_capture* capture, bitoscope_sink* sink,
                                    bitoscope_reply* reply);

/* IMPLEMENT: Optional: properties of a facet the host set (setProperties), on the SDK's main thread. The SDK has
 * checked every value against your property list already - its type, its limits, an enumeration's
 * items, and "cannot change during a capture" - and takes a request whole or not at all. `capturing`
 * is 1 while this device's logic analyzer captures (from an accepted START to the STOP after it).
 * BITOSCOPE_OK: the values become current. Anything else refuses the whole request, and the previous
 * values stay; give the reason with bitoscope_reply_message() and the property it is about with
 * bitoscope_reply_refused_property() (without one, the request's first value is named).
 * A LOGIC ANALYZER's apply function is called only WHILE IT CAPTURES, with values that may change
 * during a capture: they take effect at once. Between captures the SDK takes the values itself, and
 * they reach you at the next START, in bitoscope_capture's properties. ANY OTHER facet's apply
 * function is called for every accepted request: its values take effect at once. */
typedef int (*bitoscope_apply_properties_fn)(void* session, const bitoscope_property_value* values,
                                             size_t num_values, int capturing, bitoscope_reply* reply);

// IMPLEMENT: A preset for one channel in the host's waveform view: the name on the lead and its colour. Passed as arrays.
typedef struct bitoscope_channel_preset {
  uint32_t channel;     /* IMPLEMENT: 0 to num_channels - 1, each channel at most once */
  const char* name;     /* IMPLEMENT: NULL or "": none */
  const char* color_id; /* IMPLEMENT: one of the host's 32 channel colours, "color-1" to "color-32"; NULL or "": none */
} bitoscope_channel_preset;

/* IMPLEMENT: Fill this in (start from all zeros) and pass it to bitoscope_add_logic_analyzer(). The SDK copies
 * it, so it can live on the stack. */
typedef struct bitoscope_logic_analyzer {
  uint32_t num_channels;                /* IMPLEMENT: channels the device has, 1 to 32 */
  const uint64_t* sample_rates_milli_hz; /* IMPLEMENT: the rates it can capture at */
  size_t num_sample_rates;
  bitoscope_capture_fn capture;
  /* IMPLEMENT: Optional from here on: leave them 0. */
  bitoscope_device_bytes device_bytes; /* IMPLEMENT: every capture's bytes, unless prepare describes others */
  bitoscope_prepare_fn prepare; /* IMPLEMENT: NULL: every START that passes the SDK's checks is captured */
  /* IMPLEMENT: The highest rate for each number of selected channels: entry i is the ceiling for i + 1
   * channels - exactly num_channels entries, each at least your lowest listed rate, so every number
   * of channels has a rate. A START above the ceiling for its number of channels is refused before
   * your prepare function. NULL and 0: every rate works with every number of channels. */
  const uint64_t* rate_ceilings_milli_hz;
  size_t num_rate_ceilings;
  uint64_t default_sample_rate_milli_hz; // IMPLEMENT: the rate a user interface starts with; 0, or one you did not list: your first listed rate
  const bitoscope_channel_preset* channel_presets; /* IMPLEMENT: for the host's waveform view; NULL: none */
  size_t num_channel_presets;
  bitoscope_apply_properties_fn apply_properties; /* IMPLEMENT: NULL: every checked request is taken */
  /* IMPLEMENT: Why your device has its rate ceilings, shown after the SDK's refusal of a rate above one ("above
   * 2 Gbit/s it loses samples"). NULL: the refusal names the ceiling only. */
  const char* rate_ceiling_reason;
} bitoscope_logic_analyzer;

#define bitoscope_add_logic_analyzer(device, analyzer) \
  bitoscope_add_logic_analyzer_sized((device), (analyzer), sizeof(*(analyzer)))

#define BITOSCOPE_CAPTURE_OVER (-120)

int bitoscope_write_samples(bitoscope_sink* sink, const void* samples, uint64_t num_samples);

int bitoscope_write_samples_now(bitoscope_sink* sink, const void* samples, uint64_t num_samples);

void* bitoscope_begin_samples(bitoscope_sink* sink, uint64_t max_samples, int wait);
int bitoscope_commit_samples(bitoscope_sink* sink, uint64_t num_samples);

uint64_t bitoscope_max_chunk_samples(const bitoscope_sink* sink);

uint64_t bitoscope_slot_bytes(const bitoscope_sink* sink);

uint64_t bitoscope_sample_bytes(const bitoscope_sink* sink, uint64_t num_samples);

int bitoscope_stopping(const bitoscope_sink* sink);

int bitoscope_capture_failed(const bitoscope_sink* sink);

void bitoscope_fail_capture(bitoscope_sink* sink, const char* reason);

void bitoscope_device_gone(bitoscope_sink* sink, const char* reason);

int bitoscope_set_device_bytes(bitoscope_sink* sink, const bitoscope_device_bytes* bytes);

int bitoscope_ring_fill(const bitoscope_sink* sink, uint64_t* high_water_slots, uint64_t* slot_count);

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

const char* bitoscope_usb_error_text(void);

void* bitoscope_usb_libusb_context(bitoscope_usb_device* usb);

void* bitoscope_usb_libusb_handle(bitoscope_usb_device* usb);

/* IMPLEMENT: A silence_limit_ms of bitoscope_usb_stream that is never reached. */
#define BITOSCOPE_USB_NO_SILENCE_LIMIT 0xFFFFFFFFu

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
  /* IMPLEMENT: Bytes of each transfer, a multiple of 1024 up to 16 MiB. 0: about 2 s of your device's data,
   * 16 KiB to 1 MiB. */
  uint32_t transfer_bytes;
  uint32_t transfers; /* IMPLEMENT: transfers kept in flight, 2 to 64; 0: 8 */
  /* IMPLEMENT: No transfer completing for this long fails the capture as stalled. 0: 3 transfer periods, at
   * least 1 s, and none in a buffered capture; BITOSCOPE_USB_NO_SILENCE_LIMIT: never. */
  uint32_t silence_limit_ms;
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

/* IMPLEMENT: The facet a signal generator is listed as: the facet name bitoscope_add_property() takes for its
 * properties. */
#define BITOSCOPE_SIGNAL_GENERATOR_FACET "siggen"

/* IMPLEMENT: A waveform in a command: its name and its parameters exactly as the host sent them. Valid during
 * the call it is passed to. */
typedef struct bitoscope_waveform {
  const char* name;   /* IMPLEMENT: "square", "sine", ...; never NULL. "" in a start: what the last configure set */
  const void* params; /* IMPLEMENT: read a square wave's with bitoscope_read_square_wave(); NULL: none */
  size_t params_size;
} bitoscope_waveform;

typedef struct bitoscope_square_wave {
  uint64_t frequency_milli_hz; /* IMPLEMENT: never 0 */
  uint32_t duty_percent;       /* IMPLEMENT: 0 to 100 */
} bitoscope_square_wave;

int bitoscope_read_square_wave(const bitoscope_waveform* waveform, bitoscope_square_wave* square,
                               bitoscope_reply* reply);

/* IMPLEMENT: Start: turns the outputs in `channel_mask` on with `waveform` (an empty name: what configure set).
 * Configure: sets their waveform without turning them on. BITOSCOPE_OK: done; anything else: failed.
 * Either way bitoscope_reply_message(reply, ...) gives the host a message: what was done, or why not. */
typedef int (*bitoscope_generator_output_fn)(void* session, uint32_t channel_mask,
                                             const bitoscope_waveform* waveform, bitoscope_reply* reply);

/* IMPLEMENT: Stop: turns the outputs in `channel_mask` off. The status and the reply are as above. */
typedef int (*bitoscope_generator_stop_fn)(void* session, uint32_t channel_mask, bitoscope_reply* reply);

/* IMPLEMENT: Fill this in (start from all zeros) and pass it to bitoscope_add_signal_generator(). The SDK copies
 * it and the waveform names, so it can live on the stack. */
typedef struct bitoscope_signal_generator {
  uint32_t num_channels; /* IMPLEMENT: outputs the device has, 1 to 32 */
  bitoscope_generator_output_fn start;
  bitoscope_generator_stop_fn stop;
  bitoscope_generator_output_fn configure;
  /* IMPLEMENT: Optional from here on: leave them 0. */
  const char* const* waveforms;    /* IMPLEMENT: the waveform names the outputs can make, e.g. "square" */
  size_t num_waveforms;
  uint64_t min_frequency_milli_hz; /* IMPLEMENT: advisory bounds, for a user interface */
  uint64_t max_frequency_milli_hz;
  int supports_synchronized_start; /* IMPLEMENT: 1: several outputs in one start begin as one atomic operation */
  bitoscope_apply_properties_fn apply_properties; /* IMPLEMENT: NULL: every checked request is taken */
} bitoscope_signal_generator;

#define bitoscope_add_signal_generator(device, generator) \
  bitoscope_add_signal_generator_sized((device), (generator), sizeof(*(generator)))

/* IMPLEMENT: Handles one command to your facet: `payload` is the command as the host sent it (your facet's own
 * format). Return BITOSCOPE_OK (done), BITOSCOPE_UNKNOWN_COMMAND (the host gets a refusal), or
 * anything else (failed). Add a message or a payload to the answer with bitoscope_reply_*(). */
typedef int (*bitoscope_command_fn)(void* session, const void* payload, size_t payload_size,
                                    bitoscope_reply* reply);

typedef struct bitoscope_facet {
  const char* name; /* IMPLEMENT: e.g. "vendor.power_supply" */
  bitoscope_command_fn command;
  /* IMPLEMENT: Optional from here on: leave them 0. */
  uint32_t version_major; /* IMPLEMENT: the facet's own version; 0 counts as 1 */
  uint32_t version_minor;
  const char* const* capabilities; /* IMPLEMENT: names of optional features, for the host */
  size_t num_capabilities;
  const void* info; /* IMPLEMENT: a description in your facet's own format, sent to the host as is */
  size_t info_size;
  /* IMPLEMENT: 0: the facet is the same in every protocol version. N: this registration is used only when the
   * host and the driver agree on protocol major version N, and there it replaces a registration of
   * the same name and facet version made with 0. Registering one makes the driver offer protocol
   * version N. Leave it 0 unless a new protocol version changed this facet. */
  uint32_t protocol_major;
  bitoscope_apply_properties_fn apply_properties; /* IMPLEMENT: NULL: every checked request is taken */
} bitoscope_facet;

#define bitoscope_add_facet(device, facet) bitoscope_add_facet_sized((device), (facet), sizeof(*(facet)))

int bitoscope_reply_payload(bitoscope_reply* reply, const void* payload, size_t payload_size);

int bitoscope_reply_refused_property(bitoscope_reply* reply, const char* id);

int bitoscope_send_event(const char* facet, const void* payload, size_t payload_size);

/* IMPLEMENT: A property's type. */
#define BITOSCOPE_PROPERTY_INTEGER 0     /* IMPLEMENT: an int64 */
#define BITOSCOPE_PROPERTY_DECIMAL 1     /* IMPLEMENT: fixed point: an int64 with `digits` decimals (1.60 V is 160) */
#define BITOSCOPE_PROPERTY_BOOLEAN 2     /* IMPLEMENT: on or off */
#define BITOSCOPE_PROPERTY_ENUMERATION 3 /* IMPLEMENT: one of a list of items: names, or numbers in the unit */

/* IMPLEMENT: What a value measures: only the quantity, never milli or mega - the host chooses the prefix. */
#define BITOSCOPE_UNIT_NONE 0
#define BITOSCOPE_UNIT_VOLT 1
#define BITOSCOPE_UNIT_SECOND 2 /* IMPLEMENT: a time or a period */
#define BITOSCOPE_UNIT_HERTZ 3
#define BITOSCOPE_UNIT_PERCENT 4
#define BITOSCOPE_UNIT_SAMPLES 5

/* IMPLEMENT: How a boolean is shown. */
#define BITOSCOPE_BOOLEAN_ON_OFF 0
#define BITOSCOPE_BOOLEAN_ENABLE_DISABLE 1

/* IMPLEMENT: One property. Fill it in (start from all zeros) and pass it to bitoscope_add_property(); the SDK
 * copies it and every text and array it points to. */
typedef struct bitoscope_property {
  const char* id;   /* IMPLEMENT: never shown; unique in the facet: how a value names its property */
  const char* name; /* IMPLEMENT: shown to the user */
  int type;         /* IMPLEMENT: BITOSCOPE_PROPERTY_... */
  /* IMPLEMENT: Optional from here on: leave them 0. */
  const char* help;  /* IMPLEMENT: a tooltip */
  const char* group; // IMPLEMENT: the named group it sits in, e.g. "Outputs/PWM output 0" (groups nest with '/'); NULL or "": at the top
  int unit;          /* IMPLEMENT: BITOSCOPE_UNIT_... */
  int changeable_during_capture; /* IMPLEMENT: 1: the host may set it while the logic analyzer captures */
  uint32_t digits;   /* IMPLEMENT: a decimal's decimals, or a number enumeration's; at most 18 */
  /* IMPLEMENT: An integer's or a decimal's limits, inclusive, in its own encoding (fixed point for a decimal). */
  int has_minimum;
  int64_t minimum;
  int has_maximum;
  int64_t maximum;
  int64_t default_value; // IMPLEMENT: an integer's, decimal's (fixed point) or boolean's (0 or 1) default; outside the limits: nearest limit
  int boolean_style;     /* IMPLEMENT: BITOSCOPE_BOOLEAN_ON_OFF or BITOSCOPE_BOOLEAN_ENABLE_DISABLE */
  /* IMPLEMENT: An enumeration's items: names, OR numbers in the unit with `digits` (500 MHz in hertz with 0
   * digits: 500000000). Set one of the two arrays. */
  const char* const* item_names;
  const int64_t* item_numbers;
  size_t num_items;
  uint32_t default_index; /* IMPLEMENT: an enumeration's default item; past the end: the first */
} bitoscope_property;

#define bitoscope_add_property(device, facet_name, property) \
  bitoscope_add_property_sized((device), (facet_name), (property), sizeof(*(property)))

const bitoscope_property_value* bitoscope_find_property_value(const bitoscope_property_value* values,
                                                              size_t num_values, const char* id);

int bitoscope_add_logic_analyzer_sized(bitoscope_device* device, const bitoscope_logic_analyzer* analyzer,
                                       size_t analyzer_size);
int bitoscope_add_signal_generator_sized(bitoscope_device* device, const bitoscope_signal_generator* generator,
                                         size_t generator_size);
int bitoscope_add_facet_sized(bitoscope_device* device, const bitoscope_facet* facet, size_t facet_size);
int bitoscope_add_property_sized(bitoscope_device* device, const char* facet_name,
                                 const bitoscope_property* property, size_t property_size);

#ifdef __cplusplus
}
#endif

#endif

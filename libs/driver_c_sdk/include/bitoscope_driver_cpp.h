/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bitoscope_driver.h"

namespace bitoscope::plugin {

// IMPLEMENT: How your device's bytes hold its samples: the layout (bitoscope_driver.h).
enum class Layout : int {
  sampleMajor = BITOSCOPE_SAMPLE_MAJOR,
  channelMajor = BITOSCOPE_CHANNEL_MAJOR,
};

// IMPLEMENT: The bytes your device sends (bitoscope_device_bytes): write them as they come, and the SDK keeps the
// capture's channels, in mask order.
struct DeviceBytes {
  Layout layout = Layout::sampleMajor;
  uint32_t channelMask = 0;  // IMPLEMENT: the channels the bytes carry, the capture's and any others; 0: every one
  uint32_t strideBytes = 0;  // IMPLEMENT: from one sample (one round) to the next; 0: as few as the channels take
};

// IMPLEMENT: What a value measures: only the quantity, never milli or mega.
enum class Unit : int {
  none = BITOSCOPE_UNIT_NONE,
  volt = BITOSCOPE_UNIT_VOLT,
  second = BITOSCOPE_UNIT_SECOND,
  hertz = BITOSCOPE_UNIT_HERTZ,
  percent = BITOSCOPE_UNIT_PERCENT,
  samples = BITOSCOPE_UNIT_SAMPLES,
};

enum class BooleanStyle : int {
  onOff = BITOSCOPE_BOOLEAN_ON_OFF,
  enableDisable = BITOSCOPE_BOOLEAN_ENABLE_DISABLE,
};

// IMPLEMENT: The four types; limits are inclusive, nullopt is no limit. A default outside the limits becomes the
// nearest limit.
struct IntegerType {
  std::optional<int64_t> minimum;
  std::optional<int64_t> maximum;
  int64_t defaultValue = 0;
};
// IMPLEMENT: Fixed point: an int64 with `digits` decimals (1.60 V with 2 digits is 160); limits and default too.
struct DecimalType {
  uint32_t digits = 0;
  std::optional<int64_t> minimum;
  std::optional<int64_t> maximum;
  int64_t defaultValue = 0;
};
struct BooleanType {
  bool defaultValue = false;
  BooleanStyle style = BooleanStyle::onOff;
};
// IMPLEMENT: Items are names, OR numbers in the property's unit with `digits`: fill one of the two.
struct EnumerationType {
  std::vector<std::string> names;
  std::vector<int64_t> numbers;
  uint32_t digits = 0;
  uint32_t defaultIndex = 0;  // IMPLEMENT: past the end: the first item
};
using PropertyType = std::variant<IntegerType, DecimalType, BooleanType, EnumerationType>;

struct Property {
  std::string id;    // IMPLEMENT: never shown; unique in the facet
  std::string name;  // IMPLEMENT: shown
  PropertyType type;
  Unit unit = Unit::none;
  bool changeableDuringCapture = false;
  std::string help;  // IMPLEMENT: a tooltip
};

struct PropertyNode;

// IMPLEMENT: A named group (e.g. "PWM output 0"). Its name may not contain '/'. A group with no property in it
// is not listed.
struct PropertyGroup {
  std::string name;
  std::vector<PropertyNode> nodes;
};

struct PropertyNode {
  std::variant<Property, PropertyGroup> node;
};

// IMPLEMENT: One property's value. Which field counts is its type: `integer` for an integer and a decimal (fixed
// point), `boolean`, or `enumIndex` (the chosen item's index).
struct PropertyValue {
  std::string id;
  int64_t integer = 0;
  bool boolean = false;
  uint32_t enumIndex = 0;
};

// IMPLEMENT: Why an applyProperties() refused a request, and which property it is about (empty: the request's
// first value).
struct PropertyRefusal {
  std::string propertyId;
  std::string reason;
};

// IMPLEMENT: A preset for one channel in the host's waveform view (bitoscope_channel_preset).
struct ChannelPreset {
  uint32_t channel = 0;
  std::string name;
  std::string colorId;  // IMPLEMENT: "color-1" to "color-32"; empty: none
};

// IMPLEMENT: What the host asked for (bitoscope_capture).
struct Capture {
  uint64_t sampleRateMilliHz = 0;
  uint32_t channelMask = 0;
  uint32_t numCapturedChannels = 0;  // IMPLEMENT: the channels in a sample: the bits channelMask has set
  uint64_t sampleLimit = 0;  // IMPLEMENT: 0: until STOP
  bool buffered = false;
  // IMPLEMENT: The current value of EVERY property of the analyzer, defaults included, by id.
  std::map<std::string, PropertyValue, std::less<>> properties;
};

struct RingFill {
  uint64_t highWaterSlots = 0;
  uint64_t slotCount = 0;
};

class Sink {
 public:
  explicit Sink(bitoscope_sink* sink) : sink_(sink) {}

  bool write(std::span<const uint8_t> samples, uint64_t numSamples);
  bool writeNow(std::span<const uint8_t> samples, uint64_t numSamples);
  std::span<uint8_t> begin(uint64_t maxSamples, bool wait);
  bool commit(uint64_t numSamples);
  uint64_t maxChunkSamples() const;
  uint64_t sampleBytes(uint64_t numSamples) const;
  uint64_t slotBytes() const;
  bool stopping() const;
  bool failed() const;
  void fail(std::string_view reason);
  void deviceGone(std::string_view reason);
  bool setDeviceBytes(const DeviceBytes& bytes);
  std::optional<RingFill> ringFill() const;

  bitoscope_sink* get() const { return sink_; }

 private:
  bitoscope_sink* sink_;
};

class UsbDevice;

// IMPLEMENT: How streamUsb() streams your device's bulk IN endpoint into the sink (bitoscope_usb_stream).
struct UsbStream {
  UsbDevice* device = nullptr;  // IMPLEMENT: required: your device, as UsbDevice::open() gave it
  uint8_t endpoint = 0;         // IMPLEMENT: required: the bulk IN endpoint, 0x81 say
  // IMPLEMENT: Optional: called once every transfer is in flight: send your device's start command; an error
  // fails the capture with it as the reason. Empty: you started your device before the call.
  std::function<std::expected<void, std::string>()> started;
  // IMPLEMENT: Optional: called once the stream has ended and no transfer is in flight, unless the device is gone:
  // send your device's stop command; an error is logged as a warning and changes nothing. Empty: nothing is called.
  std::function<std::expected<void, std::string>()> stop;
  // IMPLEMENT: Optional: endpoint read empty before transfers are queued; only with started. Bytes still arriving
  // after about 1 s of that fail the capture at sample 0.
  bool drain = false;
  uint64_t prerollSamples = 0;  // IMPLEMENT: thrown away at the start, rounded up to whole units
  uint32_t transferBytes = 0;   // IMPLEMENT: a multiple of 1024 up to 16 MiB; 0: about 2 s of data, 16 KiB to 1 MiB
  uint32_t transfers = 0;       // IMPLEMENT: 2 to 64 in flight; 0: 8
  // IMPLEMENT: No completion for this long fails as stalled. 0: 3 transfer periods, at least 1 s, none when
  // buffered; BITOSCOPE_USB_NO_SILENCE_LIMIT: never.
  uint32_t silenceLimitMs = 0;
  uint64_t lagLimitBytes = 0;  // IMPLEMENT: delivery this far behind the sample clock fails; 0 or buffered: unchecked
  // IMPLEMENT: Optional: turns whole units of device bytes (unitBytes each) into the capture's DeviceBytes,
  // unitSamples samples a unit; `samples` has room for exactly as many units. An error fails the capture with it
  // as reason.
  std::function<std::expected<void, std::string>(std::span<const uint8_t> deviceBytes, std::span<uint8_t> samples)>
      convert;
  uint32_t unitBytes = 0;
  uint32_t unitSamples = 0;
  std::string deviceName;  // IMPLEMENT: names your device in messages; empty: "USB device"
};

void streamUsb(Sink& sink, const UsbStream& stream);

// IMPLEMENT: What a logic analyzer has, as identify describes it (bitoscope_logic_analyzer's fields of the same
// names): its channels and rates are required, the rest optional.
struct LogicAnalyzerInfo {
  uint32_t numChannels = 0;
  std::vector<uint64_t> sampleRatesMilliHz;
  DeviceBytes deviceBytes;  // IMPLEMENT: every capture's bytes, unless prepare() describes others
  std::vector<PropertyNode> properties;
  // IMPLEMENT: The highest rate for each number of channels: entry i is for i + 1 channels, exactly numChannels
  // entries, each at least the lowest rate; empty: none.
  std::vector<uint64_t> rateCeilingsMilliHz;
  uint64_t defaultSampleRateMilliHz = 0;  // IMPLEMENT: the rate a user interface starts with; 0 or unlisted: the first
  std::vector<ChannelPreset> channelPresets;
  std::string rateCeilingReason;  // IMPLEMENT: why the ceilings, shown after the SDK's refusal; empty: none
};

class LogicAnalyzer {
 public:
  virtual ~LogicAnalyzer() = default;

  // IMPLEMENT: Values the host set that take effect at once, on the SDK's main thread: called only WHILE A
  // CAPTURE RUNS (`capturing` true), with values of properties changeable during a capture. Between
  // captures the SDK takes the values itself; they reach prepare() and capture() in
  // Capture::properties. An error refuses the whole request and the previous values stay. The
  // default takes them.
  virtual std::expected<void, PropertyRefusal> applyProperties(std::span<const PropertyValue> values, bool capturing);

  // IMPLEMENT: Prepares a START, on the SDK's main thread, after the SDK's own checks and before capture(): an
  // error refuses the START with the error as its reason, and capture() is not called. It may describe
  // the capture's bytes (sink.setDeviceBytes()) and read sink.maxChunkSamples(); it does not write. The
  // default starts every capture.
  virtual std::expected<void, std::string> prepare(const Capture& capture, Sink& sink);

  // IMPLEMENT: The capture, on the SDK's capture thread: writes samples until a write answers false (the capture
  // is over), then returns {}. When the device fails, it returns the reason as its error (or calls
  // sink.fail(), or sink.deviceGone() when it is unplugged); an exception is such a failure, with its
  // what() as the reason. Returning before a write answered false fails the capture too, and the SDK
  // says so. Before its first write it may describe the capture's bytes (sink.setDeviceBytes()).
  virtual std::expected<void, std::string> capture(const Capture& capture, Sink& sink) = 0;
};

// IMPLEMENT: What the generator offers, as identify describes it; sent to the host in the facet's info.
struct SignalGeneratorInfo {
  uint32_t numChannels = 0;  // IMPLEMENT: outputs; output c is bit c of a command's channel mask. At least 1.
  std::vector<std::string> supportedWaveforms;  // IMPLEMENT: e.g. "square"
  uint64_t minFrequencyMilliHz = 0;             // IMPLEMENT: advisory bounds, for a user interface
  uint64_t maxFrequencyMilliHz = 0;
  bool supportsSynchronizedStart = false;  // IMPLEMENT: several outputs start as one atomic operation
  std::vector<PropertyNode> properties;    // IMPLEMENT: optional: e.g. a group per output
};

// IMPLEMENT: A waveform in a command: its name and its parameters exactly as the host sent them (a serialized
// <Name>WaveParams; decodeSquareWave() reads a square wave's).
struct Waveform {
  std::string name;  // IMPLEMENT: "square", "sine", ...; empty in a START: what the last CONFIGURE set
  std::vector<uint8_t> params;
};

struct SquareWave {
  uint64_t frequencyMilliHz = 0;
  uint32_t dutyPercent = 0;  // IMPLEMENT: 0 to 100
};

std::expected<SquareWave, std::string> decodeSquareWave(std::span<const uint8_t> params);

class SignalGenerator {
 public:
  virtual ~SignalGenerator() = default;

  // IMPLEMENT: Turns the outputs in `channelMask` on with `waveform` (an empty name: what CONFIGURE set).
  virtual std::expected<std::string, std::string> start(uint32_t channelMask, const Waveform& waveform) = 0;
  // IMPLEMENT: Turns the outputs in `channelMask` off.
  virtual std::expected<std::string, std::string> stop(uint32_t channelMask) = 0;
  // IMPLEMENT: Sets the outputs' waveform without turning them on.
  virtual std::expected<std::string, std::string> configure(uint32_t channelMask, const Waveform& waveform) = 0;

  // IMPLEMENT: Values the host set, checked by the SDK, on the SDK's main thread: they take effect at once.
  // `capturing`: the device's logic analyzer captures. An error refuses the whole request and the
  // previous values stay. The default takes them.
  virtual std::expected<void, PropertyRefusal> applyProperties(std::span<const PropertyValue> values, bool capturing);
};

// IMPLEMENT: The device as identify describes it: the SDK made it with the found device's id. Open may set its name
// and serial number again; its facets are identify's.
class Device {
 public:
  explicit Device(bitoscope_device* device) : device_(device) {}

  bool setName(std::string_view name);
  bool setSerialNumber(std::string_view serialNumber);
  bool add(const LogicAnalyzerInfo& analyzer);
  bool add(const SignalGeneratorInfo& generator);

  bitoscope_device* get() const { return device_; }

 private:
  bitoscope_device* device_;
};

// IMPLEMENT: What open returns: the objects that run the device's facets for the session, and whatever they share
// (the opened device). The SDK destroys it when the session ends. Each facet identify described needs its object;
// the same object for the whole session.
class Session {
 public:
  virtual ~Session() = default;

  virtual LogicAnalyzer* logicAnalyzer();
  virtual SignalGenerator* signalGenerator();
};

// IMPLEMENT: The transport a device connects through (bitoscope_transport): what DriverDescription declares,
// and what a found device was found on (FoundDevice::transport()).
enum class Transport : int {
  none = BITOSCOPE_TRANSPORT_NONE,
  usb = BITOSCOPE_TRANSPORT_USB,
  serial = BITOSCOPE_TRANSPORT_SERIAL,
  network = BITOSCOPE_TRANSPORT_NETWORK,
};

// IMPLEMENT: The connection the host gave a serial or network driver (bitoscope_connection); nullopt: not given.
struct Connection {
  std::optional<std::string> networkAddress;
  std::optional<uint16_t> networkPort;
  std::optional<std::string> serialPort;
};

// IMPLEMENT: Where a USB device was found (bitoscope_usb_location).
struct UsbLocation {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;  // IMPLEMENT: the interface its UsbId claims
  uint8_t bus = 0;
  uint8_t address = 0;          // IMPLEMENT: changes each time the device enumerates
  std::vector<uint8_t> ports;   // IMPLEMENT: the port path from the root hub: 4, 2 for id "usb-1-4.2"
};

// IMPLEMENT: Where a serial device was found (bitoscope_serial_location).
struct SerialLocation {
  std::string port;  // IMPLEMENT: the name you open the port by: "/dev/ttyACM0", "COM3"
};

// IMPLEMENT: Where a network device was found (bitoscope_network_location).
struct NetworkLocation {
  std::string address;  // IMPLEMENT: a host name or an IP address
  uint16_t port = 0;    // IMPLEMENT: 0: none
};

// IMPLEMENT: Where a device was found, one alternative per transport; std::monostate: no place given.
using Location = std::variant<std::monostate, UsbLocation, SerialLocation, NetworkLocation>;

// IMPLEMENT: The device an identify, open or held check is about (bitoscope_found_device).
struct FoundDevice {
  std::string id;  // IMPLEMENT: the user selects the device by it
  Location location;
  std::string appearanceKey;  // IMPLEMENT: changes each time the device appears anew; empty: none given
  Connection connection;

  Transport transport() const;
  const UsbLocation* usb() const { return std::get_if<UsbLocation>(&location); }
  const SerialLocation* serial() const { return std::get_if<SerialLocation>(&location); }
  const NetworkLocation* network() const { return std::get_if<NetworkLocation>(&location); }
};

// IMPLEMENT: One device your enumerate found: its id, and optionally where it is and its appearance key (a new
// value makes the SDK identify it again). Identify and open get them back in FoundDevice.
struct EnumeratedDevice {
  std::string id;
  Location location;
  std::string appearanceKey;
};

// IMPLEMENT: Why identify or open cannot use the device now (bitoscope_driver.h, BITOSCOPE_BUSY and the rest).
enum class DeviceProblem { busy, noPermission, firmwareLoading, notSupported, other };

// IMPLEMENT: What identify or open returns when it cannot use the device: the reason, and a text for other.
struct DeviceFailure {
  DeviceProblem problem = DeviceProblem::other;
  std::string detail;
};

// IMPLEMENT: The link speed of a USB device (BITOSCOPE_USB_SPEED_...).
enum class UsbSpeed : int {
  unknown = BITOSCOPE_USB_SPEED_UNKNOWN,
  low = BITOSCOPE_USB_SPEED_LOW,
  full = BITOSCOPE_USB_SPEED_FULL,
  high = BITOSCOPE_USB_SPEED_HIGH,
  super = BITOSCOPE_USB_SPEED_SUPER,
  superPlus = BITOSCOPE_USB_SPEED_SUPER_PLUS,
};

// IMPLEMENT: A control transfer's setup: bit 7 of requestType set reads from the device.
struct UsbSetup {
  uint8_t requestType = 0;
  uint8_t request = 0;
  uint16_t value = 0;
  uint16_t index = 0;
};

// IMPLEMENT: Why a transfer failed: status BITOSCOPE_USB_TIMEOUT, BITOSCOPE_DEVICE_GONE or BITOSCOPE_ERROR; the
// text names the cause; transferred: bytes that went through before it.
struct UsbError {
  int status = BITOSCOPE_ERROR;
  std::string text;
  size_t transferred = 0;
};

class UsbDevice {
 public:
  static std::expected<UsbDevice, DeviceFailure> open(const FoundDevice& device);

  UsbDevice() = default;
  explicit UsbDevice(bitoscope_usb_device* device) : device_(device) {}
  UsbDevice(UsbDevice&& other) noexcept : device_(std::exchange(other.device_, nullptr)) {}
  UsbDevice& operator=(UsbDevice&& other) noexcept;
  UsbDevice(const UsbDevice&) = delete;
  UsbDevice& operator=(const UsbDevice&) = delete;
  ~UsbDevice() { close(); }

  void close();
  bool isOpen() const { return device_ != nullptr; }
  UsbSpeed speed() const;
  std::expected<size_t, UsbError> bulkOut(uint8_t endpoint, std::span<const uint8_t> data, uint32_t timeoutMs);
  std::expected<size_t, UsbError> bulkIn(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs);
  std::expected<size_t, UsbError> control(const UsbSetup& setup, std::span<uint8_t> data, uint32_t timeoutMs);
  void* libusbContext() const;
  void* libusbHandle() const;

  bitoscope_usb_device* get() const { return device_; }

 private:
  bitoscope_usb_device* device_ = nullptr;
};

// IMPLEMENT: What open reports while it works (BITOSCOPE_PROGRESS_...).
enum class ProgressCode : int {
  other = BITOSCOPE_PROGRESS_OTHER,
  firmwareLoading = BITOSCOPE_PROGRESS_FIRMWARE_LOADING,
  waitingForDevice = BITOSCOPE_PROGRESS_WAITING_FOR_DEVICE,
};

class Progress {
 public:
  explicit Progress(bitoscope_progress* progress) : progress_(progress) {}

  bool report(ProgressCode code, std::string_view detail);

 private:
  bitoscope_progress* progress_;
};

// IMPLEMENT: One USB product your driver drives (bitoscope_usb_id); genericName is optional (empty: displayName).
struct UsbId {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  std::string genericName;
};

// IMPLEMENT: Not USB: every device present (bitoscope_enumerate_fn); an error: devices could not be looked for.
using EnumerateFunction = std::function<std::expected<std::vector<EnumeratedDevice>, std::string>(const Connection&)>;
// IMPLEMENT: Describes the found device in `device`: its name, optionally its serial number, and its facets.
using IdentifyFunction = std::function<std::expected<void, DeviceFailure>(const FoundDevice& found, Device& device)>;
// IMPLEMENT: Opens the device for the session, after identify described it, and returns the session that runs its
// facets; may set `device`'s name and serial number again and report progress meanwhile.
using OpenFunction = std::function<std::expected<std::unique_ptr<Session>, DeviceFailure>(
    const FoundDevice& found, Device& device, Progress& progress)>;
// IMPLEMENT: True when another program holds the device; asked without using it.
using HeldCheckFunction = std::function<bool(const FoundDevice& device)>;

// IMPLEMENT: Your driver (bitoscope_driver): what is required, and the optional pieces left empty for the SDK's defaults.
struct DriverDescription {
  std::string displayName;  // IMPLEMENT: required: your driver's name, as users see it
  // IMPLEMENT: required: your driver's version as X.Y.Z with an optional suffix: "1.0.0", "1.2.3-beta", "2.0.3-alpha01"
  std::string version;
  Transport transport = Transport::none;  // IMPLEMENT: required: Transport::usb when you give usbIds
  std::vector<UsbId> usbIds;  // IMPLEMENT: required, or enumerate: how your devices are found
  EnumerateFunction enumerate;
  IdentifyFunction identify;  // IMPLEMENT: required
  OpenFunction open;          // IMPLEMENT: required
  HeldCheckFunction heldCheck;  // IMPLEMENT: optional: empty: USB devices get the SDK's own check, others count as free
};

class Driver {
 public:
  explicit Driver(DriverDescription description);
  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;

  int run(int argc, char** argv);

  const bitoscope_driver* get() const { return &driver_; }

 private:
  static int enumerate(void* user, const bitoscope_connection* connection, bitoscope_found_devices* found);
  static int identify(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                      bitoscope_reply* reply);
  static int open(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                  bitoscope_progress* progress, void** session, bitoscope_reply* reply);
  static void close(void* user, void* session);
  static int heldCheck(void* user, const bitoscope_found_device* device);

  DriverDescription description_;
  std::vector<bitoscope_usb_id> usbIds_;
  bitoscope_driver driver_{};
};

int runDriver(int argc, char** argv, DriverDescription description);

}

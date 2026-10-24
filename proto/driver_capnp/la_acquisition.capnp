# SPDX-License-Identifier: MIT

@0xe2a67dcb1c647a04;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("bitoscope::ipc::wire::la");

struct LaAcquisitionInfo {
  numChannels @0 :UInt32;
  supportedSampleRatesMilliHz @1 :List(UInt64);
  bulkRing @2 :Bool;
  rateCeilingsMilliHz @3 :List(UInt64);
  defaultSampleRateMilliHz @4 :UInt64;
  channelPresets @5 :List(ChannelPreset);
}

struct ChannelPreset {
  channel @0 :UInt32;
  name @1 :Text;
  colorId @2 :Text;
}

enum AcquisitionMode {
  streaming @0;
  buffered @1;
}

struct StartCapture {
  sampleRateMilliHz @0 :UInt64;
  channelMask @1 :UInt32;
  sampleLimit @2 :UInt64;
  mode @3 :AcquisitionMode;
  captureId @4 :UInt64;
  bulkRingName @5 :Text;
}

struct LaAcquisitionCommand {
  union {
    none @0 :Void;
    start @1 :StartCapture;
    stop @2 :Void;
    # WHY: Union member @3 (configure) was removed and is never reused: older peers still send it and it is refused.
  }
}

enum EndReason {
  unspecified @0;
  sampleLimitReached @1;
  failure @2;
}

struct CaptureEnded {
  captureId @0 :UInt64;
  reason @1 :EndReason;
  message @2 :Text;
}

struct LaAcquisitionEvent {
  union {
    none @0 :Void;
    captureEnded @1 :CaptureEnded;
  }
}

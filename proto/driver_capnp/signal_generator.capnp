# SPDX-License-Identifier: MIT

@0xe9e75d81dca25e49;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("bitoscope::ipc::wire::siggen");

struct SignalGeneratorInfo {
  numChannels @0 :UInt32;
  supportedWaveforms @1 :List(Text);
  minFrequencyMilliHz @2 :UInt64;
  maxFrequencyMilliHz @3 :UInt64;
  supportsSynchronizedStart @4 :Bool;
}

struct Waveform {
  name @0 :Text;
  params @1 :Data;
}

struct SignalGeneratorCommand {
  channelMask @0 :UInt32;
  union {
    none @1 :Void;
    start @2 :Waveform;
    stop @3 :Void;
    configure @4 :Waveform;
  }
}

struct SquareWaveParams {
  frequencyMilliHz @0 :UInt64;
  dutyPercent @1 :UInt32;
}

struct SineWaveParams {
  frequencyMilliHz @0 :UInt64;
  amplitudeMicrovolts @1 :UInt64;
  offsetMicrovolts @2 :Int64;
  phaseMilliDegrees @3 :UInt32;
}

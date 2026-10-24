# SPDX-License-Identifier: MIT

# WHY: Every union's first member is `none :Void`: an all-zero message decodes as it and is refused.
# Enum names avoid C++ macro names (ERROR, DEBUG): generated code is upper case; windows.h defines ERROR.

@0xf2165ec1225a4014;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("bitoscope::ipc::wire");

using Props = import "properties.capnp";

struct Version {
  major @0 :UInt32;
  minor @1 :UInt32;
}

struct FacetInfo {
  name @0 :Text;
  version @1 :Version;
  info @2 :Data;
  capabilities @3 :List(Text);
  unusable @4 :Text;
  majors @5 :List(UInt32);
  properties @6 :List(Props.PropertyNode);
}

enum UnavailableReason {
  none @0;
  busy @1;
  noPermission @2;
  firmwareLoading @3;
  other @4;
}

struct DeviceInfo {
  id @0 :Text;
  name @1 :Text;
  facets @2 :List(FacetInfo);
  serialNumber @3 :Text;
  unavailable @4 :UnavailableReason;
  unavailableDetail @5 :Text;
  identified @6 :Bool;
}

# WHY: FacetRequest.name stays field @0: an old host's List(Text) reads as List(FacetRequest) with names only.
struct FacetRequest {
  name @0 :Text;
  minMajor @1 :UInt32;
  maxMajor @2 :UInt32;
}

struct Handshake {
  minMajor @0 :UInt32;
  maxMajor @1 :UInt32;
  minor @2 :UInt32;
  requestedFacets @3 :List(FacetRequest);
}

struct Abbreviation {
  upper @0 :Text;
  shown @1 :Text;
  alsoWord @2 :Bool;
}

struct HandshakeReply {
  accepted @0 :Bool;
  major @1 :UInt32;
  minor @2 :UInt32;
  message @3 :Text;
  device @4 :DeviceInfo;
  sdkVersion @5 :Text;
  abbreviations @6 :List(Abbreviation);
  unavailable @7 :UnavailableReason;
  pluginVersion @8 :Text;
}

struct FacetCommand {
  facet @0 :Text;
  payload @1 :Data;
}

struct FacetReply {
  ok @0 :Bool;
  message @1 :Text;
  payload @2 :Data;
  refusedProperty @3 :Text;
}

struct FacetEvent {
  facet @0 :Text;
  payload @1 :Data;
}

enum LogLevel {
  unspecified @0;
  debugLevel @1;
  infoLevel @2;
  warningLevel @3;
  errorLevel @4;
}

struct LogEvent {
  level @0 :LogLevel;
  message @1 :Text;
  timestampNs @2 :Int64;
  source @3 :Text;
}

struct DeviceList {
  devices @0 :List(DeviceInfo);
  supportsWatch @1 :Bool;
  driverName @2 :Text;
  pluginVersion @3 :Text;
}

struct DeviceListed {
  device @0 :DeviceInfo;
}

struct DeviceDetached {
  id @0 :Text;
  reason @1 :Text;
}

struct WatchFailed {
  reason @0 :Text;
}

struct Refusal {
  message @0 :Text;
  unknownMember @1 :UInt16;
}

struct ListAgain {}

enum OpenProgressCode {
  other @0;
  firmwareLoading @1;
  waitingForDevice @2;
}

struct OpenProgress {
  code @0 :OpenProgressCode;
  detail @1 :Text;
}

struct Envelope {
  requestId @0 :UInt64;
  union {
    none @1 :Void;
    handshake @2 :Handshake;
    handshakeReply @3 :HandshakeReply;
    facetCommand @4 :FacetCommand;
    facetReply @5 :FacetReply;
    facetEvent @6 :FacetEvent;
    logEvent @7 :LogEvent;
    deviceList @8 :DeviceList;
    deviceListed @9 :DeviceListed;
    deviceDetached @10 :DeviceDetached;
    refusal @11 :Refusal;
    setProperties @12 :Props.SetProperties;
    watchFailed @13 :WatchFailed;
    listAgain @14 :ListAgain;
    openProgress @15 :OpenProgress;
  }
}

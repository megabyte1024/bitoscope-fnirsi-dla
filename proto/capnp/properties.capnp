# SPDX-License-Identifier: MIT

@0xea3e02cca477b208;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("bitoscope::ipc::wire::props");

enum Unit {
  none @0;
  volt @1;
  second @2;
  hertz @3;
  percent @4;
  samples @5;
}

enum BooleanStyle {
  onOff @0;
  enableDisable @1;
}

struct Bound {
  union {
    none @0 :Void;
    value @1 :Int64;
  }
}

struct IntegerSpec {
  min @0 :Bound;
  max @1 :Bound;
  defaultValue @2 :Int64;
}

struct DecimalSpec {
  digits @0 :UInt32;
  min @1 :Bound;
  max @2 :Bound;
  defaultValue @3 :Int64;
}

struct BooleanSpec {
  defaultValue @0 :Bool;
  style @1 :BooleanStyle;
}

struct EnumItem {
  union {
    none @0 :Void;
    name @1 :Text;
    number @2 :Int64;
  }
}

struct EnumerationSpec {
  items @0 :List(EnumItem);
  digits @1 :UInt32;
  defaultIndex @2 :UInt32;
}

struct Property {
  id @0 :Text;
  name @1 :Text;
  help @2 :Text;
  changeableDuringCapture @3 :Bool;
  unit @4 :Unit;
  type :union {
    none @5 :Void;
    integer @6 :IntegerSpec;
    decimal @7 :DecimalSpec;
    boolean @8 :BooleanSpec;
    enumeration @9 :EnumerationSpec;
  }
}

struct PropertyGroup {
  name @0 :Text;
  nodes @1 :List(PropertyNode);
}

struct PropertyNode {
  union {
    none @0 :Void;
    group @1 :PropertyGroup;
    property @2 :Property;
  }
}

struct PropertyValue {
  id @0 :Text;
  union {
    none @1 :Void;
    integer @2 :Int64;
    boolean @3 :Bool;
    enumIndex @4 :UInt32;
  }
}

struct SetProperties {
  facet @0 :Text;
  values @1 :List(PropertyValue);
}

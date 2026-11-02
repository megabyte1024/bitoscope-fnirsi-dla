# SPDX-License-Identifier: MIT
@0xa0f3e31e92bf7659;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("bitoscope::ipc::wire::firmware");

struct FirmwareUpdateInfo {
  running @0 :Running;
  model @1 :Text;
  modelChoices @2 :List(Text);
  fileDescription @3 :Text;
  imageNames @4 :List(Text);
  estimatedSeconds @5 :UInt32;

  enum Running {
    program @0;
    bootloader @1;
  }
}

struct BeginUpload {
  totalBytes @0 :UInt64;
  fileName @1 :Text;
}

struct UploadPiece {
  uploadId @0 :UInt32;
  offset @1 :UInt64;
  bytes @2 :Data;
}

struct Check {
  uploadId @0 :UInt32;
  model @1 :Text;
  confirmUnknownFile @2 :Bool;
  confirmOtherModel @3 :Bool;
}

struct Start {
  uploadId @0 :UInt32;
  model @1 :Text;
  confirmUnknownFile @2 :Bool;
  confirmOtherModel @3 :Bool;
}

struct Cancel {
  confirmCannotCapture @0 :Bool;
}

struct FirmwareUpdateCommand {
  union {
    none @0 :Void;
    beginUpload @1 :BeginUpload;
    uploadPiece @2 :UploadPiece;
    check @3 :Check;
    start @4 :Start;
    cancel @5 :Cancel;
    startProgram @6 :Void;
    status @7 :Void;
  }
}

struct UploadReply {
  uploadId @0 :UInt32;
}

struct CheckReply {
  verdict @0 :Verdict;
  fileModel @1 :Text;
  sha256 @2 :Data;
  message @3 :Text;

  enum Verdict {
    fits @0;
    unreadable @1;
    modelUnknown @2;
    unknownFile @3;
    otherModel @4;
  }
}

struct CancelReply {
  answer @0 :Answer;

  enum Answer {
    accepted @0;
    needsConfirmation @1;
    tooLate @2;
  }
}

struct FirmwareUpdateProgress {
  phase @0 :Phase;
  imageName @1 :Text;
  bytesSent @2 :UInt64;
  bytesTotal @3 :UInt64;
  attempt @4 :UInt32;
  attempts @5 :UInt32;
  resends @6 :UInt32;

  enum Phase {
    preparing @0;
    sending @1;
    finishing @2;
  }
}

struct FirmwareUpdateFinished {
  outcome @0 :Outcome;
  message @1 :Text;
  canCapture @2 :Bool;
  attempts @3 :UInt32;

  enum Outcome {
    done @0;
    cancelled @1;
    failed @2;
    needsReplug @3;
  }
}

struct FirmwareUpdateEvent {
  union {
    none @0 :Void;
    progress @1 :FirmwareUpdateProgress;
    finished @2 :FirmwareUpdateFinished;
  }
}

struct StatusReply {
  running @0 :Bool;
  last @1 :FirmwareUpdateEvent;
}

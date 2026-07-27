#!/usr/bin/env python3
"""Generate the event-bus wire conformance vectors.

These bytes are the cross-language authority (D8 of docs/dev/EVENT_BUS_DECISIONS.md):
the C codec, the Go reference client (slice 9), and any third-language client are
all held to them.

This generator exists so that authority is *independent*. If the vectors were
produced by the C encoder and then verified by re-running the C encoder, a bug
in that encoder would be baked into the table and pass its own check — the
vectors would be a copy of the implementation rather than a statement about the
wire. So the layout is re-declared here from the spec, byte offset by byte
offset, using struct.pack. Two implementations built from the same prose, in
different languages, agreeing byte-for-byte is evidence; one implementation
agreeing with itself is not.

Regeneration is a deliberate, reviewable act: run this, commit the diff, and the
diff shows exactly which bytes moved.

    scripts/gen_bus_wire_vectors.py > src/tests/fixtures/bus/wire_vectors.tsv
"""

import struct
import sys

HDR_LEN = 64
MAGIC = 0x30535542  # "BUS0" little-endian
WIRE_VERSION = 2

# hdr_flags
F_INLINE = 0x0001
F_ARENA = 0x0002
F_NOTIFICATION = 0x0004
F_REQUEST = 0x0008
F_REPLY = 0x0010
F_CANCEL = 0x0020
F_CONTROL = 0x0040

# reserved event kinds
K_CAPABILITY_ABSENT = 4
K_OVERFLOW = 5
K_MODULE_BASE = 256

FIELD_ORDER = [
    "flags", "ver", "kind", "principal", "corr",
    "seq", "lts", "pref", "plen", "src", "dst",
]


def encode(f):
    """Pack a frame from the spec's byte layout.

    Declared here as one format string rather than field-by-field writes, so the
    offsets are visible as a whole and a reordering cannot hide in a diff:

        <   little-endian, no padding
        I   0  magic          H  4  hdr_flags     H  6  wire_version
        I   8  event_kind     I 12  principal_ref
        Q  16  correlation_id Q 24  seq           Q 32  logical_ts
        Q  40  payload_ref    I 48  payload_len
        I  52  src_handle     I 56  dst_handle    I 60  generation
    """
    blob = struct.pack(
        "<IHHIIQQQQIIII",
        MAGIC,
        f["flags"], f["ver"],
        f["kind"], f["principal"],
        f["corr"], f["seq"], f["lts"], f["pref"],
        f["plen"], f["src"], f["dst"], f["gen"],
    )
    if len(blob) != HDR_LEN:
        # Not an assert: `python -O` strips those, and a generator whose own
        # safety net can be compiled out would emit silently wrong vectors.
        raise SystemExit(f"layout drifted: packed {len(blob)} bytes, expected {HDR_LEN}")
    return blob


def frame(**kw):
    f = {
        "flags": 0, "ver": WIRE_VERSION, "kind": K_MODULE_BASE,
        "principal": 0x11223344, "corr": 0, "seq": 0,
        "lts": 0x0102030405060708, "pref": 0, "plen": 0, "src": 7, "dst": 0,
        "gen": 0,
    }
    f.update(kw)
    return f


CORR = 0xDEADBEEFCAFEF00D
INLINE_AT = HDR_LEN  # in-slot payloads start just past the header

VECTORS = [
    # Every message pattern, so the table covers the routing contract and not
    # just the happy path.
    ("notification.empty", frame(flags=F_NOTIFICATION)),
    ("notification.inline.32",
     frame(flags=F_NOTIFICATION | F_INLINE, plen=32, pref=INLINE_AT)),
    ("notification.arena.65536",
     frame(flags=F_NOTIFICATION | F_ARENA, plen=65536, pref=3, gen=5)),
    ("request.arena",
     frame(flags=F_REQUEST | F_ARENA, corr=CORR, plen=4096, pref=9, gen=2)),
    ("reply.inline",
     frame(flags=F_REPLY | F_INLINE, corr=CORR, plen=16, pref=INLINE_AT,
           seq=4242, dst=7)),
    ("cancel", frame(flags=F_CANCEL, corr=CORR)),
    ("capability_absent",
     frame(flags=F_REPLY, kind=K_CAPABILITY_ABSENT, corr=CORR, seq=99, dst=7)),

    # An overflow notice is an ordinary seq-stamped frame carrying the control
    # flag (D6), not a side channel with a shape of its own.
    ("control.overflow",
     frame(flags=F_NOTIFICATION | F_CONTROL | F_INLINE, kind=K_OVERFLOW,
           plen=16, pref=INLINE_AT, seq=5000, dst=7)),

    # D4's independence proof: identical frames but for the placement flag, at
    # the provisional 192-byte inline budget. Nothing in the frame encodes the
    # threshold that chose the placement, so re-tuning inline_budget in slice 12
    # cannot invalidate a committed vector.
    ("budget.inline.192",
     frame(flags=F_NOTIFICATION | F_INLINE, plen=192, pref=INLINE_AT)),
    ("budget.arena.192",
     frame(flags=F_NOTIFICATION | F_ARENA, plen=192, pref=1, gen=1)),
]


# Negative vectors: byte sequences a conforming decoder must refuse, with the
# result name it must report. Built by mutating a known-good frame at the byte
# level, so they exercise the decoder's own guards rather than the encoder's.
#
# Without these the table was all-OK rows, which would let a decoder that
# accepted everything pass conformance — and "the Go client agrees with the C
# client" would then be a statement about two permissive parsers.
def negatives():
    good = bytearray(encode(frame(flags=F_NOTIFICATION)))
    out = []

    def mut(name, expect, edits):
        b = bytearray(good)
        for off, fmt, val in edits:
            struct.pack_into(fmt, b, off, val)
        out.append((name, expect, b))

    b = bytearray(good); b[0] ^= 0xFF
    out.append(("neg.bad_magic", "ERR_MAGIC", b))

    b = bytearray(good); b[60] = 1  # generation!=0 on a non-arena (notification) frame
    out.append(("neg.generation_on_non_arena", "ERR_FLAGS", b))

    mut("neg.version_above", "ERR_VERSION", [(6, "<H", WIRE_VERSION + 1)])
    mut("neg.version_zero", "ERR_VERSION", [(6, "<H", 0)])
    mut("neg.no_pattern", "ERR_FLAGS", [(4, "<H", 0)])
    mut("neg.two_patterns", "ERR_FLAGS", [(4, "<H", F_REQUEST | F_REPLY), (16, "<Q", CORR)])
    mut("neg.unknown_flag_bit", "ERR_FLAGS", [(4, "<H", F_NOTIFICATION | 0x8000)])
    mut("neg.payload_without_placement", "ERR_FLAGS", [(48, "<I", 32)])
    mut("neg.both_placements", "ERR_FLAGS",
        [(4, "<H", F_NOTIFICATION | F_INLINE | F_ARENA), (48, "<I", 32)])
    mut("neg.placement_without_payload", "ERR_FLAGS", [(4, "<H", F_NOTIFICATION | F_INLINE)])
    mut("neg.ref_without_payload", "ERR_PAYLOAD_LEN", [(40, "<Q", 4096)])
    mut("neg.notification_with_correlation", "ERR_CORRELATION", [(16, "<Q", 5)])
    mut("neg.request_without_correlation", "ERR_CORRELATION", [(4, "<H", F_REQUEST)])
    return out


def fields_str(f):
    return ("flags=0x%04x;ver=%u;kind=%u;principal=%u;corr=0x%016x;seq=%u;"
            "lts=0x%016x;pref=0x%016x;plen=%u;src=%u;dst=%u;gen=%u") % (
        f["flags"], f["ver"], f["kind"], f["principal"], f["corr"],
        f["seq"], f["lts"], f["pref"], f["plen"], f["src"], f["dst"], f["gen"])


def main():
    out = sys.stdout
    out.write("# event-bus wire vectors v%d — generated by scripts/gen_bus_wire_vectors.py.\n"
              % WIRE_VERSION)
    out.write("# Independent of any client implementation: the layout is re-declared\n")
    out.write("# from the spec here, so agreement between this table and a codec is\n")
    out.write("# evidence about the wire rather than a codec agreeing with itself.\n")
    out.write("# Regenerate deliberately; review the byte diff.\n")
    out.write("# name\texpect\thex\tfields\n")

    seen = set()
    for name, f in VECTORS:
        if name in seen:
            raise SystemExit(f"duplicate vector name {name}")
        seen.add(name)
        out.write("%s\tOK\t%s\t%s\n" % (name, encode(f).hex(), fields_str(f)))

    # Negative rows carry no field column — there is no valid frame to describe.
    # A dash keeps every row four columns wide for any consumer.
    for name, expect, blob in negatives():
        if name in seen:
            raise SystemExit(f"duplicate vector name {name}")
        seen.add(name)
        if len(blob) != HDR_LEN:
            raise SystemExit(f"negative vector {name} is {len(blob)} bytes")
        out.write("%s\t%s\t%s\t-\n" % (name, expect, bytes(blob).hex()))


if __name__ == "__main__":
    main()

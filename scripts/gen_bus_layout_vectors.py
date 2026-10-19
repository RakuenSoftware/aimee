#!/usr/bin/env python3
"""Generate the event-bus layout vectors.

The wire vectors (gen_bus_wire_vectors.py) freeze the 64-byte frame. They say
nothing about the shared-memory regions, the attach handshake, or the AMOD
envelope, so until this table existed a client in a new language had to read
those layouts out of a reference implementation. This table is that layout
stated once, independently of any implementation: every offset, size, magic,
version and reserved number a client needs to attach, map, poll, publish,
serve and call. The C, Go and Rust clients are each held to it by their own
tests, in both directions: every row must match the implementation, and every
layout constant the implementation's test names must have a row.

The values are re-declared here from the specification and the decision record
(docs/proposals/done/event-bus-wire-spec.md, docs/dev/EVENT_BUS_DECISIONS.md),
not read from any header, so agreement is evidence rather than a copy.

Regeneration is a deliberate, reviewable act:

    scripts/gen_bus_layout_vectors.py > src/tests/fixtures/bus/layout_vectors.tsv
"""

import sys


def le_magic(text):
    """A four-character magic as the little-endian u32 the regions store."""
    if len(text) != 4:
        raise SystemExit(f"magic {text!r} is not four characters")
    return int.from_bytes(text.encode("ascii"), "little")


# (name, value, note). Names are dotted: <structure>.<kind>.<field>.
ROWS = []


def row(name, value, note=""):
    ROWS.append((name, value, note))


def struct(prefix, size, fields):
    """Declare a packed structure: fields are (name, width) in order, laid out
    without padding, and must exactly fill `size`."""
    off = 0
    for name, width in fields:
        row(f"{prefix}.off.{name}", off)
        off += width
    if off != size:
        raise SystemExit(f"{prefix}: fields fill {off} bytes, declared {size}")
    row(f"{prefix}.size", size)


# ---- versions --------------------------------------------------------------
row("version.spec", 1, "control region spec_version")
row("version.layout", 1, "control region layout_version")
row("version.wire", 3, "frame wire_version")
row("version.amod", 1, "AMOD envelope version")

# ---- frame -------------------------------------------------------------------
row("frame.size", 64, "the 64-byte header; byte offsets are in wire_vectors.tsv")
row("frame.magic", le_magic("BUS0"))

# ---- reserved event kinds ----------------------------------------------------
for name, value in [
    ("attach_request", 1), ("attach_reply", 2), ("error", 3),
    ("capability_absent", 4), ("overflow", 5), ("producer_reaped", 6),
    ("epoch_change", 7), ("capability_denied", 8), ("lease_release", 9),
    ("module_base", 256),
]:
    row(f"kind.{name}", value)

# ---- control region (bus_control_t; D1, D4, D12) -----------------------------
row("control.magic", le_magic("BCTL"))
struct("control", 96, [
    ("magic", 4), ("spec_version", 4), ("layout_version", 4), ("flags", 4),
    ("slot_size", 4), ("inline_budget", 4), ("queue_capacity", 4),
    ("lease_view_slots", 4),
    ("arena_size", 8), ("host_epoch", 8), ("host_heartbeat", 8),
    ("lease_view_off", 8), ("reserved1", 32),
])
row("control.flag.lease_view", 0x1, "D12: a lease view follows the header")

# ---- lease view (D12) --------------------------------------------------------
row("lease_view.slots", 4096)
struct("lease_view.entry", 16, [("generation", 4), ("len", 4), ("offset", 8)])
row("lease_view.region_bytes", 96 + 4096 * 16, "control header + view")

# ---- queue-pair region (bus_qpair_hdr_t) -------------------------------------
# The header comments call this "QPBR", but the stored bytes spell "QBPR".
row("qpair.magic", le_magic("QBPR"))
struct("qpair", 96, [
    ("magic", 4), ("slot_size", 4), ("capacity", 4), ("data_credits", 4),
    ("control_credits", 4), ("control_lost", 4), ("inbound_off", 4),
    ("outbound_off", 4), ("client_heartbeat", 8), ("reserved", 56),
])
row("qpair.ring_align", 64, "each ring starts on a 64-byte boundary after the header")

# ---- ring (bus_ring_shared_t) ------------------------------------------------
row("ring.magic", le_magic("RING"))
row("ring.off.magic", 0)
row("ring.off.slot_size", 4)
row("ring.off.capacity", 8)
row("ring.off.mask", 12)
row("ring.off.slots_off", 16)
row("ring.off.head", 64, "producer index, its own cache line")
row("ring.off.tail", 128, "consumer index, its own cache line")
row("ring.size", 192, "header; the slot array starts here")

# ---- arena region (bus_arena_hdr_t) ------------------------------------------
row("arena.magic", le_magic("BARN"))
struct("arena", 16, [("magic", 4), ("reserved", 4), ("size", 8)])

# ---- attach handshake (bus_attach_request_t / bus_attach_reply_t) -------------
row("attach_request.magic", le_magic("ABRQ"))
struct("attach_request", 16, [
    ("magic", 4), ("wire_version_min", 2), ("wire_version_max", 2),
    ("principal_class", 4), ("principal_ref", 4),
])
row("attach_reply.magic", le_magic("ABRP"))
struct("attach_reply", 48, [
    ("magic", 4), ("status", 4), ("handle_id", 4), ("wire_version", 4),
    ("slot_size", 4), ("inline_budget", 4), ("queue_capacity", 4),
    ("reserved", 4), ("arena_size", 8), ("host_epoch", 8),
])
for name, value in [("ok", 0), ("denied_policy", 1), ("denied_version", 2),
                    ("denied_noslot", 3), ("protocol", 4)]:
    row(f"attach_status.{name}", value)
row("attach.descriptors", 3, "SCM_RIGHTS order: control, arena, queue pair")
row("attach.fd.control", 0)
row("attach.fd.arena", 1)
row("attach.fd.qpair", 2)

# ---- lease release payload (D12) ---------------------------------------------
struct("lease_release", 8, [("lease_id", 4), ("generation", 4)])

# ---- AMOD module envelope ----------------------------------------------------
row("amod.magic", le_magic("AMOD"))
struct("amod", 40, [
    ("magic", 4), ("version", 2), ("header_len", 2), ("operation", 2),
    ("status", 2), ("stage_id", 4), ("flags", 4), ("body_len", 4),
    ("deadline_ns", 8), ("trace_id", 8),
])
row("amod.max_body", 16 * 1024 * 1024)
row("amod.op.invoke", 1)
row("amod.op.result", 2)
for name, value in [("ok", 0), ("capability_absent", 1), ("cancelled", 2),
                    ("deadline_exceeded", 3), ("invalid_request", 4), ("internal", 5)]:
    row(f"amod.status.{name}", value)


def main():
    out = sys.stdout
    out.write("# event-bus layout vectors — generated by scripts/gen_bus_layout_vectors.py.\n")
    out.write("# Region, ring, attach, lease-view and AMOD layouts, declared independently of\n")
    out.write("# any client implementation. The C, Go and Rust clients are each held to it.\n")
    out.write("# Regenerate deliberately; review the diff.\n")
    out.write("# name\tvalue\n")
    seen = set()
    for name, value, _note in ROWS:
        if name in seen:
            raise SystemExit(f"duplicate layout row {name}")
        seen.add(name)
        text = f"0x{value:08x}" if name.endswith(".magic") else str(value)
        out.write(f"{name}\t{text}\n")


if __name__ == "__main__":
    main()

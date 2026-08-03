/* bus_capture.h: the event-bus capture stream and observational replay (D10).
 *
 * The capture stream is exactly the host's seq order: it is fed by the
 * governance/audit tap (D6/D9), so every seq-stamped event — including the
 * host-generated overflow and producer_reaped notices — is recorded once, in
 * order. From that one ordered stream the delivered stream is derivable (D6),
 * which is the property the later governance-and-capture tree depends on.
 *
 * The wire format is normative (D10), frozen by this slice's tests: a fixed file
 * header, then length-prefixed CRC-checked records carrying the verbatim frame
 * header and the materialized payload bytes. Payloads are materialized because
 * an arena region does not outlive the host; a reader uses the inline bytes and
 * must never dereference a captured payload_ref.
 *
 * Observational replay re-presents the ordered stream for inspection. Nothing is
 * re-executed, so it is exact by construction. Module replay (re-driving a module
 * against recorded inbound events) is a later tree, not this one.
 */
#ifndef AIMEE_BUS_CAPTURE_H
#define AIMEE_BUS_CAPTURE_H 1

#include <stddef.h>
#include <stdint.h>

#include <aimee/core/event_bus/bus_host.h> /* bus_tap_fn, bus_frame_t */

/* File header, 64 bytes. All integers little-endian. magic is the 8 ASCII bytes
 * "AIMEECAP". */
#define BUS_CAPTURE_MAGIC          "AIMEECAP"
#define BUS_CAPTURE_FORMAT_VERSION 1
#define BUS_CAPTURE_HEADER_LEN     64

/* Record types. */
typedef enum
{
   BUS_CAP_EVENT = 0,
   BUS_CAP_OVERFLOW = 1,
   BUS_CAP_PRODUCER_REAPED = 2,
   BUS_CAP_EPOCH_CHANGE = 3
} bus_capture_record_type_t;

/* record_flags bit 0: the payload bytes were materialized inline. */
#define BUS_CAP_F_MATERIALIZED 0x0001u

/* A growable in-memory capture sink. The format on disk and in memory is
 * identical; a file writer is a thin wrapper a later tree can add. */
typedef struct
{
   uint8_t *buf;
   size_t len;
   size_t cap;
   uint64_t first_seq;
   uint64_t last_seq;
   int have_first;
   int header_written;
   uint32_t spec_version;
   uint32_t layout_version;
   uint64_t host_epoch;
   int broken; /* an allocation failed; the stream is abandoned */
} bus_capture_t;

/* Terminal classification of a stream, decided from the bytes alone (D10). */
typedef enum
{
   BUS_CAPTURE_COMPLETE = 0, /* ended cleanly with an epoch_change */
   BUS_CAPTURE_OPEN,         /* valid capture of a still-running host */
   BUS_CAPTURE_TRUNCATED,    /* cut mid-write; last good seq/offset reported */
   BUS_CAPTURE_CORRUPT       /* a structural contradiction before the end */
} bus_capture_status_t;

/* Result of reading a stream. */
typedef struct
{
   bus_capture_status_t status;
   uint64_t records;      /* records successfully parsed */
   uint64_t last_good_seq;
   size_t offending_off;  /* byte offset of the record that failed, if any */
   int rule;              /* which classification rule fired (for diagnostics) */
} bus_capture_report_t;

/* One replayed record handed to a callback. payload points into the stream and
 * is valid for the duration of the callback. */
typedef struct
{
   bus_capture_record_type_t type;
   bus_frame_t frame;
   const uint8_t *payload;
   uint32_t payload_len;
} bus_capture_event_t;

typedef void (*bus_capture_cb)(void *ctx, const bus_capture_event_t *ev);

/* ---- writing ---- */

/* Initialise a sink. The spec/layout/epoch go in the file header. */
void bus_capture_init(bus_capture_t *c, uint32_t spec_version, uint32_t layout_version,
                      uint64_t host_epoch);

/* The tap function: register with bus_host_set_tap(h, bus_capture_tap, c). It
 * classifies overflow/producer_reaped/epoch_change by event_kind and records
 * everything else as an event. */
void bus_capture_tap(void *ctx, const bus_frame_t *frame, const uint8_t *payload,
                     uint32_t payload_len);

/* Free the sink's buffer. */
void bus_capture_free(bus_capture_t *c);

/* ---- reading / replay ---- */

/* Classify a byte stream and, if not corrupt/truncated before it, replay each
 * record to cb in order. cb may be NULL to classify without replaying. */
bus_capture_report_t bus_capture_read(const uint8_t *buf, size_t len, bus_capture_cb cb,
                                      void *ctx);

const char *bus_capture_status_name(bus_capture_status_t s);

#endif /* AIMEE_BUS_CAPTURE_H */

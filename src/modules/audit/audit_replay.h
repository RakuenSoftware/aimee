/* audit_replay.h: read an audit-on-bus capture file and re-present the
 * governed-action rows it recorded.
 *
 * The event bus exists for auditability and record+replay; audit_bus.c records
 * every governed-action row to a per-session capture file. This is the operator
 * side: given such a file, replay it — observationally, nothing re-executed — and
 * print each row in the order it happened, with the stream's terminal
 * classification (was it a clean/open capture, or truncated/corrupt).
 *
 * It lives in aimee-server (the only shipping binary that links the bus): the
 * capture reader is bus code, so a CLI tool would widen the D7 blast radius. This
 * header deliberately pulls in NO bus header, so a caller (server_main) can invoke
 * the tool without itself referencing the bus.
 */
#ifndef AIMEE_AUDIT_REPLAY_H
#define AIMEE_AUDIT_REPLAY_H 1

#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Read the capture file at `path`, replay it to `out` (one line per recorded
    * governed-action row, in order), and print a trailer with the stream status
    * and the number of audit rows replayed. `out` may be NULL to classify/validate
    * without printing.
    *
    * Returns 0 if the stream parsed as a valid capture (COMPLETE or OPEN), -1 if
    * the file could not be read, and -2 if the stream is TRUNCATED or CORRUPT
    * (still prints what was recoverable before the break). */
   int audit_bus_replay_print(const char *path, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_REPLAY_H */

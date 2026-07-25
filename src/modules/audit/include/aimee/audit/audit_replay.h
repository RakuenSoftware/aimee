/* audit_replay.h: read an audit-on-bus capture file and re-present the
 * governed-action rows it recorded.
 *
 * The event bus exists for auditability and record+replay; obs_bus.c records
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

struct cJSON;

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
   int obs_bus_replay_print(const char *path, FILE *out);

   /* JSON forms for the /v1/audit endpoints (both return caller-owned cJSON).
    * audit_replay_to_json returns {status, total, count, offset, rows:[{seq,
    * verdict,actor,tool,mode,reason,task_id,args_hash,command}...]} for the
    * capture file at `path`, or NULL if the file could not be read. Only the
    * [offset, offset+limit) slice is materialized (limit<=0 = all) so a caller can
    * keep the response bounded; `total` still reports every row in the stream.
    * audit_replay_capture_list returns an array of {name, bytes} for the capture
    * files in `dir` (empty array, never NULL). Declared with `struct cJSON *` so
    * this header pulls in no cJSON (nor bus) dependency on its includers. */
   struct cJSON *audit_replay_to_json(const char *path, long offset, long limit);
   struct cJSON *audit_replay_capture_list(const char *dir);

   /* 1 iff `name` is a safe capture-file basename: no path separator (no
    * traversal) and the audit-capture naming convention. A client-supplied file
    * name MUST pass this before it is joined to the capture directory. */
   int audit_replay_valid_basename(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_REPLAY_H */

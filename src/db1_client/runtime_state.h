/* db1/runtime_state.h: per-machine runtime state.
 *
 * Stores the subset of `memory_runtime_state` rows that describe the
 * *local* machine: memory query counters, maintenance cadence keys, and
 * other per-host telemetry. pgvector runtime coordination lives in DB2's
 * kb_runtime_state (see db2/kb_runtime_state.h) because it belongs to
 * aimee-kb, not the host.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_RUNTIME_STATE_H
#define DEC_DB1_RUNTIME_STATE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Insert or replace (state_key, state_value). 0 on success, -1 on error. */
   int db1_runtime_state_set(const char *key, const char *value);

   /* Read value for `key`. 0 on hit, -1 on miss or error. Writes empty
    * string when the stored value is empty. */
   int db1_runtime_state_get(const char *key, char *out, size_t out_len);

   /* Parse the stored integer value for `key`, add `delta`, and persist the
    * result. Missing/empty rows are treated as zero. Writes the new value to
    * `new_value_out` when non-NULL. Returns 0 on success. */
   int db1_runtime_state_add_int(const char *key, int delta, int *new_value_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_RUNTIME_STATE_H */

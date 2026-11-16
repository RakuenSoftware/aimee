/* db1/runtime_state.h: per-machine runtime state.
 *
 * Stores the subset of `memory_runtime_state` rows that describe the
 * *local* machine: memory query counters, maintenance cadence keys, and
 * other per-host telemetry. pgvector runtime coordination lives in KB_STORE's
 * kb_runtime_state (see kb_store/kb_runtime_state.h) because it belongs to
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

   /* Optional lookup: 0 on hit (including empty), 1 only on an explicit store
    * missing reply, -1 on transport/store/malformed-reply failure. Clears out
    * unless a value was successfully read; never treats failure as absence. */
   int db1_runtime_state_lookup(const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_RUNTIME_STATE_H */

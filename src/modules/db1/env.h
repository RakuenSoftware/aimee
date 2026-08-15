/* db1/env.h: user-local environment state.
 *
 * Two small independent tables:
 *   - env_capabilities: cached results of environment probes (what tools
 *     are installed, what GPU is present, etc.) keyed by probe name.
 *   - maintenance_state: last-run bookkeeping for the memory maintenance
 *     cycle (memory_maintenance.c), keyed by cycle name.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_ENV_H
#define DEC_DB1_ENV_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* --- env_capabilities --- */

   typedef struct
   {
      char key[128];
      char value[512];
      char detected_at[32];
   } db1_env_capability_t;

   /* Insert or replace a capability row. Returns 0 on success, -1 on error. */
   int db1_env_capability_set(const char *key, const char *value);

   /* Read one capability value by key. Returns 1 when found, 0 when absent,
    * or -1 on backend error. */
   int db1_env_capability_get(const char *key, char *value_out, size_t value_len,
                              char *detected_at_out, size_t detected_at_len);

   /* Read all capability rows ordered by key. Fills up to `max` entries and
    * returns the count written, or -1 on error. */
   int db1_env_capability_list(db1_env_capability_t *out, int max);

   /* --- maintenance_state --- */

   typedef struct
   {
      int present;
      char last_run_at[32];
      int64_t last_memory_count;
      int last_changes;
      double last_elapsed_ms;
      char last_summary_json[2048];
   } db1_maintenance_state_t;

   /* Load the maintenance state row for `key`. Sets out->present=0 if the
    * row does not exist. Returns 0 on success (present or absent), -1 on
    * a backend error. */
   int db1_maintenance_state_load(const char *key, db1_maintenance_state_t *out);

   /* Upsert a maintenance state row for `key`. Returns 0 on success. */
   int db1_maintenance_state_save(const char *key, const db1_maintenance_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_ENV_H */

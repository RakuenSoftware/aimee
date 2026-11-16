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

/* The capability row's widths, stated here because they cross the module
   boundary and the stage cannot see the caller's buffers. */
#define DB1_ENV_KEY_LEN   128
#define DB1_ENV_VALUE_LEN 512
#define DB1_ENV_TS_LEN    32

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* --- env_capabilities --- */

   typedef struct
   {
      char key[DB1_ENV_KEY_LEN];
      char value[DB1_ENV_VALUE_LEN];
      char detected_at[DB1_ENV_TS_LEN];
   } db1_env_capability_t;

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_ENV_H */

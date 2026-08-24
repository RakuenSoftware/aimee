/* db1/caches.h: user-local runtime caches.
 *
 * Two independent key/value caches:
 *   - context cache: memoised memory-context assemblies, hash-keyed with TTL.
 *   - context snapshots: records which memories were surfaced in which
 *     sessions so effectiveness can be computed against DB1 session outcomes.
 *   - agent result cache: last result per (role, prompt) for recent-repeat
 *     agent invocations.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_CACHES_H
#define DEC_DB1_CACHES_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_CONTEXT_SNAPSHOT_SESSION_LEN 128

   /* --- Context cache --- */

   /* Read a cached assembly by hash. Returns 0 on hit, -1 on miss or expired. */
   int db1_context_cache_get(const char *hash, char *out, size_t out_len);

   /* Insert or replace a cached assembly. No-op on error. */
   void db1_context_cache_put(const char *hash, const char *output);

   /* Delete every row in the context cache. */
   void db1_context_cache_invalidate(void);

   /* --- Context snapshots --- */

   int db1_context_snapshot_insert(const char *session_id, int64_t memory_id,
                                   double relevance_score);
   int db1_context_snapshot_count_memories_with_min_samples(int min_samples);
   int db1_context_snapshot_list_memory_ids_with_min_samples(int min_samples, int64_t *out,
                                                             int max);
   int db1_context_snapshot_count_for_memory(int64_t memory_id);
   int db1_context_snapshot_list_sessions_for_memory(int64_t memory_id,
                                                     char (*out)[DB1_CONTEXT_SNAPSHOT_SESSION_LEN],
                                                     int max);
   int db1_context_snapshot_has_memory(int64_t memory_id);

   /* --- Agent result cache ---
    *
    * Only use this for roles where the prompt is complete input. Roles that
    * inspect repository state must bypass it; identical prompts can refer to
    * different worktree contents. */

   /* Returns a malloc'd copy of the cached result for (role, prompt),
    * or NULL if absent. Caller frees. */
   char *db1_agent_cache_get(const char *role, const char *prompt);

   /* Insert or replace (role, prompt) → result. No-op on error. */
   void db1_agent_cache_put(const char *role, const char *prompt, const char *result);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_CACHES_H */

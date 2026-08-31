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

/* One activation row, rendered as "<memory_id> <last_turn>". Two int64 decimals
 * plus a separator; 48 leaves room without inviting a wider payload. */
#define DB1_CONTEXT_ACTIVATION_ROW_LEN 48

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

   /* Record an injection against the turn it happened on. The turn-less form
    * above stays for callers that only need "this was sampled"; anything that
    * has to answer "how many turns ago" writes through this one. */
   int db1_context_snapshot_insert_turn(const char *session_id, int64_t memory_id,
                                        double relevance_score, int64_t turn_index);

   /* One conversation's activation state: for each unit ever injected in this
    * session, the most recent turn it fired on, highest turn first.
    *
    * Read once per turn, not once per candidate -- a gate placed in front of
    * retrieval has to cost far less than the retrieval it guards, and a round
    * trip per candidate would not. Returns rows written, or -1. */
   int db1_context_snapshot_activation(const char *session_id,
                                       char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max);

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

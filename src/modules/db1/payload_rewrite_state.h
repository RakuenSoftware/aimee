/* db1/payload_rewrite_state.h: DB1 storage for per-session prompt-cache rewrite metadata. */
#ifndef DB1_PAYLOAD_REWRITE_STATE_H
#define DB1_PAYLOAD_REWRITE_STATE_H 1

#include <stdint.h>

typedef struct
{
   char session_id[128];
   int64_t payload_epoch;
   int64_t compaction_epoch;
   char last_prefix_hash[128];
   int last_payload_tokens;
   char last_rewrite_at[32];
   int deferred_rewrite_count;
   int consecutive_deferred_count;
   int bytes_saved_pending;
   char rewrite_reason[128];
   char updated_at[32];
} payload_rewrite_state_t;

/* Read state for a session. Returns 0 on success, -1 if not found or error. */
int db1_payload_rewrite_state_get(const char *session_id, payload_rewrite_state_t *out);

/* Upsert state for a session. Returns 0 on success, -1 on error. */
int db1_payload_rewrite_state_set(const payload_rewrite_state_t *s);

/* Record a rewrite event: increments the appropriate counter, sets reason and timestamp.
 * deferred=1 means a cache-preserving deferral; deferred=0 means a forced rewrite.
 * Returns 0 on success. */
int db1_payload_rewrite_record(const char *session_id, int deferred, int bytes_saved,
                               int new_payload_tokens, const char *reason,
                               const char *new_prefix_hash);

#endif /* DB1_PAYLOAD_REWRITE_STATE_H */

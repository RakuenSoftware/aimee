/* db2/db2_learning.h: learning_signals + learning_proposals primitives.
 *
 * Owns SQL for the explicit-signal-capture / proposal-gate pipeline
 * documented in docs/proposals/done/learning-signals-router-phase-*.
 * The learning_signal_input_t / learning_proposal_t types live in
 * modules/learning/learning.h (the ensemble-learning types); callers
 * include "learning.h" before this. */
#ifndef DEC_DB2_LEARNING_H
#define DEC_DB2_LEARNING_H 1

#include "../modules/learning/learning.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Sweep state='pending' learning_proposals whose expires_at has
    * elapsed into state='archived' with archive_reason='expired'. Best
    * effort — failures are logged and ignored. */
   void db2_learning_proposals_archive_expired(void);

   /* Mark a learning_proposals row archived with the given reason and
    * stamp updated_at = now. Returns 0 on success, -1 on SQL /
    * connection error. */
   int db2_learning_proposal_archive(int id, const char *reason);

   /* Increment corroboration_count and stamp updated_at = now on a
    * pending learning_proposals row. Returns 0 on success, -1 on SQL /
    * connection error. */
   int db2_learning_proposal_bump_corroboration(int id);

   /* Count rows in learning_proposals where sink = ?, state = 'committed',
    * and committed_at falls within the last 7 days. Returns count, or
    * 0 on miss / DB unavailable. */
   int db2_learning_commits_in_last_7_days(const char *sink);

   /* INSERT a new learning_signals row. created_at stamps now.
    * source_session may be NULL/empty. evidence_refs_json may be NULL
    * (defaults to "[]"). Returns the new row id (>0) on success, -1 on
    * SQL / connection error. */
   int db2_learning_signal_insert(const learning_signal_input_t *input, const char *source_session);

   /* Look up the most recent state='pending' learning_proposals row
    * matching (sink, target_key, target_memory_id). Returns the id
    * (>0) on hit, 0 on miss / DB unavailable. target_key may be NULL
    * (treated as ""). */
   int db2_learning_proposal_find_pending(const char *sink, const char *target_key,
                                          int64_t target_memory_id);

   /* INSERT a new learning_proposals row with state='pending',
    * corroboration_count=1. expires_at must be a pre-formatted UTC
    * timestamp (caller picks ttl). evidence_refs may be NULL
    * (defaults to "[]"). Returns the new row id (>0) on success, -1
    * on SQL / connection error. */
   int db2_learning_proposal_insert(int signal_id, const char *sink, const char *target_key,
                                    int64_t target_memory_id, const char *action_json,
                                    const char *evidence_refs, const char *expires_at);

   /* Transition a learning_proposals row to state='committed', stamping
    * committed_at = updated_at = now. Returns 0 on success, -1 on SQL /
    * connection error. */
   int db2_learning_proposal_mark_committed(int id);

   /* Load a learning_proposals row by id into |out|. Returns 0 on hit,
    * -1 on miss / SQL / connection error. */
   int db2_learning_proposal_get(int id, learning_proposal_t *out);

   /* List learning_proposals rows filtered by |state| and |sink|. Empty
    * / NULL filters skip the corresponding WHERE clause. Rows are
    * returned in id-DESC order, capped at |max| (caller's buffer
    * size); |limit| further caps the SQL fetch (clamped to max).
    * Returns count (>=0) on success, -1 on SQL / connection error. */
   int db2_learning_proposal_list(const char *state, const char *sink, int limit,
                                  learning_proposal_t *out, int max);

   /* Settled-window proposal counts: committed and (committed + archived)
    * within |window_days|, using COALESCE(committed_at, updated_at,
    * created_at) for the timestamp. window_days must be >0. Sets
    * *committed and *terminal on success and returns 0; returns -1 on
    * bad args / SQL / connection error. Either pointer may be NULL. */
   int db2_learning_proposals_settled_counts(int window_days, int64_t *committed,
                                             int64_t *terminal);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_LEARNING_H */

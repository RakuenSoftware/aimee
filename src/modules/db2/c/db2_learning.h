/* db2/db2_learning.h: learning_signals + learning_proposals primitives.
 *
 * Owns SQL for the explicit-signal-capture / proposal-gate pipeline
 * documented in docs/proposals/done/learning-signals-router-phase-*.
 * The learning_signal_input_t / learning_proposal_t types live in
 * aimee/learning/learning.h (the ensemble-learning types); callers
 * include "learning.h" before this. */
#ifndef DEC_DB2_LEARNING_H
#define DEC_DB2_LEARNING_H 1

#include <aimee/learning/learning.h>

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

#define DB2_LEARNING_SOURCE_LEN      16
#define DB2_LEARNING_SIGNAL_TYPE_LEN 32

   /* One (source, signal_type) group with its committed-proposal count. */
   typedef struct
   {
      char source[DB2_LEARNING_SOURCE_LEN];
      char signal_type[DB2_LEARNING_SIGNAL_TYPE_LEN];
      int64_t count;
   } db2_learning_source_count_t;

   /* Committed proposals within |window_days|, grouped by the originating
    * signal's (source, signal_type). Rows are the raw provenance groups; the
    * exogenous/endogenous judgement is policy and belongs to the learning
    * module, not to SQL. `sink_or_null` filters to one sink when non-empty.
    * Uses COALESCE(committed_at, updated_at, created_at) for the timestamp,
    * matching db2_learning_proposals_settled_counts.
    * Returns rows written (>=0, capped at max), or -1 on bad args / SQL /
    * connection error. window_days must be > 0. */
   int db2_learning_committed_source_counts(int window_days, const char *sink_or_null,
                                            db2_learning_source_count_t *out, int max);

#define DB2_LEARNING_NEG_TITLE_LEN      256
#define DB2_LEARNING_NEG_DESC_LEN       1024
#define DB2_LEARNING_NEG_CORRECTION_LEN 1024
#define DB2_LEARNING_NEG_SESSION_LEN    64

   /* One negative signal that carried a correction. */
   typedef struct
   {
      int64_t id;
      char signal_type[DB2_LEARNING_SIGNAL_TYPE_LEN];
      char source[DB2_LEARNING_SOURCE_LEN];
      char title[DB2_LEARNING_NEG_TITLE_LEN];
      char description[DB2_LEARNING_NEG_DESC_LEN];
      char correction_text[DB2_LEARNING_NEG_CORRECTION_LEN];
      char source_session[DB2_LEARNING_NEG_SESSION_LEN];
   } db2_learning_negative_signal_t;

   /* Negative-polarity signals from the last |window_days| that carry a
    * non-empty correction_text, newest first. These are the only signal rows
    * that state BOTH what was asked and what should have been said, which is
    * what makes them synthesisable into a regression check.
    * Returns rows written (>=0, capped at max), or -1 on bad args / SQL /
    * connection error. window_days must be > 0. */
   int db2_learning_negative_signals_recent(int window_days, db2_learning_negative_signal_t *out,
                                            int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_LEARNING_H */

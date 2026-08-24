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

#define LEARNING_OBSERVATION_TEXT_MAX 512
#define LEARNING_REF_JSON_MAX         2048

   typedef struct
   {
      char observation_id[64];
      char scope_kind[32];
      char scope_id[128];
      char observation_type[64];
      char title[256];
      char summary[LEARNING_OBSERVATION_TEXT_MAX];
      char status[16];
      double confidence;
      char evidence_window_start[40];
      char evidence_window_end[40];
      char synthesis_policy_version[64];
      int evidence_count;
      int independent_session_count;
      char supersedes[64];
      char superseded_by[64];
      char created_at[40];
      char refreshed_at[40];
      char retired_at[40];
   } learning_observation_t;

   typedef struct
   {
      int64_t source_event_id;
      const char *source_span;
      const char *stance;
   } learning_observation_evidence_input_t;

   typedef struct
   {
      char application_id[64];
      int64_t source_event_id;
      char session_id[128];
      char scope_kind[32];
      char scope_id[128];
      char task_family[128];
      char observation_id[64];
      char procedure_artifact_id[64];
      int proposal_id;
      int retrieved;
      int rendered;
      int selected;
      int applied;
      char outcome[16];
      char failure_class[128];
      char human_correction[512];
      int64_t latency_ms;
      int tool_count;
      int turn_count;
      int64_t token_count;
      char retrieved_refs[LEARNING_REF_JSON_MAX];
      char rendered_refs[LEARNING_REF_JSON_MAX];
      char selected_refs[LEARNING_REF_JSON_MAX];
      char applied_refs[LEARNING_REF_JSON_MAX];
   } learning_application_event_t;

   /* Deterministically materialize/refresh one recurring-failure observation
    * and attach every matching source event through max_event_id. The derived
    * row becomes active only at the closed policy's 3-event/2-session floor. */
   int db2_learning_observation_refresh_recurrence(const char *observation_id, const char *role,
                                                   const char *failure_mode, const char *title,
                                                   const char *summary, int64_t max_event_id);

   /* Read-only observation surfaces. evidence ids are stable source_event_id
    * locators, ordered oldest-first. */
   int db2_learning_observation_get(const char *observation_id, learning_observation_t *out);
   int db2_learning_observation_list(const char *status, const char *scope_kind,
                                     const char *scope_id, int limit, learning_observation_t *out,
                                     int max);
   int db2_learning_observation_evidence_ids(const char *observation_id, int64_t *out, int max);
   int db2_learning_observation_set_status(const char *observation_id, const char *status);
   /* Closed-type, normalized observation refresh. The transaction upserts the
    * derived row, idempotently attaches exact event locators, and recomputes its
    * lifecycle from the effective supporting/contradicting evidence. */
   int db2_learning_observation_refresh(const char *observation_id, const char *scope_kind,
                                        const char *scope_id, const char *observation_type,
                                        const char *title, const char *summary,
                                        const char *policy_version,
                                        const learning_observation_evidence_input_t *evidence,
                                        int evidence_count, const char *supersedes);
   int db2_learning_observation_add_evidence(const char *observation_id, int64_t source_event_id,
                                             const char *source_span, const char *stance);
   /* Recompute evidence/session counts and retire observations whose effective
    * supporting evidence fell below policy after erasure/invalidation. */
   int db2_learning_observations_reconcile(void);

   /* Idempotent use/outcome attribution. Exposure stages are independent in
    * storage, while impossible orderings are rejected (application requires
    * selection, rendering, and retrieval). */
   int db2_learning_application_record(const learning_application_event_t *event);
   int db2_learning_application_get(const char *application_id, learning_application_event_t *out);

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

   /* Replace the evidence reference manifest on a pending proposal after its
    * deterministic observation is refreshed. */
   int db2_learning_proposal_refresh_evidence(int id, const char *evidence_refs_json);

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

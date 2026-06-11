/* db1/roundtable_pipeline.h: durable ledger for the agent roundtable authoring
 * pipeline (idea -> reviewed proposal -> implementation -> reviewed PR).
 *
 * Pure domain API. No backend types or handles in any signature. The ledger is
 * the source of truth for the done-bar and the human gates; it is NOT the
 * bounded, non-durable /v1/runs store. See
 * docs/proposals/.../agent-roundtable-authoring-pipeline.md (esp. section 4). */
#ifndef DEC_DB1_ROUNDTABLE_PIPELINE_H
#define DEC_DB1_ROUNDTABLE_PIPELINE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* section 1 pipeline states (persisted as text). */
#define RTP_STATE_DRAFTING            "drafting"
#define RTP_STATE_PROPOSAL_REVIEW     "proposal_review"
#define RTP_STATE_GATE1_PENDING       "gate1_pending"
#define RTP_STATE_GATE1_MERGE_PENDING "gate1_merge_pending"
#define RTP_STATE_IMPLEMENTING        "implementing"
#define RTP_STATE_PR_REVIEW           "pr_review"
#define RTP_STATE_GATE2_PENDING       "gate2_pending"
#define RTP_STATE_GATE2_MERGE_PENDING "gate2_merge_pending"
#define RTP_STATE_DONE                "done"
#define RTP_STATE_FAILED              "failed"
#define RTP_STATE_ABANDONED           "abandoned"

/* Admission classes (section 1/section 4): only one pipeline may be `active` at a time. */
#define RTP_ADMIT_ACTIVE        "active"
#define RTP_ADMIT_PARKED        "parked"
#define RTP_ADMIT_WAITING_HUMAN "waiting_human"

/* Phase identifiers. */
#define RTP_PHASE_PROPOSAL "proposal"
#define RTP_PHASE_IMPL     "impl"

/* Done-bar enum (section 3/section 6), persisted as text. */
#define RTP_DONEBAR_ZERO_BLOCKING             "zero_blocking"
#define RTP_DONEBAR_ZERO_BLOCKING_SUGGESTIONS "zero_blocking_suggestions"
#define RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS   "zero_blocking_questions_answered"

/* Pass mode/status. */
#define RTP_MODE_DRAFT  "draft"
#define RTP_MODE_REVIEW "review"

#define RTP_PASS_OPEN       "open"
#define RTP_PASS_CAPTURED   "captured"
#define RTP_PASS_DONE       "done"
#define RTP_PASS_SUPERSEDED "superseded"
#define RTP_PASS_FAILED     "failed"

/* Attempt capture status (#23/#25/#44). */
#define RTP_CAP_PENDING   "pending"
#define RTP_CAP_CAPTURED  "captured"
#define RTP_CAP_LOST      "lost_result"
#define RTP_CAP_CANCELLED "cancelled"
#define RTP_CAP_FAILED    "failed"

#define RTP_TS_LEN     32
#define RTP_HASH_LEN   72
#define RTP_SHORT_LEN  32
#define RTP_NAME_LEN   128
#define RTP_PATH_LEN   512
#define RTP_IDEA_LEN   2048
#define RTP_BRIEF_LEN  4096
#define RTP_DIGEST_LEN 4096
#define RTP_SNAP_LEN   8192
#define RTP_REASON_LEN 1024
#define RTP_RUNID_LEN  128

   typedef struct
   {
      int id;
      char idea[RTP_IDEA_LEN];
      char state[RTP_SHORT_LEN];
      char phase[RTP_SHORT_LEN];
      char admission_class[RTP_SHORT_LEN];
      int schema_version;
      char done_bar[RTP_SHORT_LEN];
      char brief[RTP_BRIEF_LEN];
      char gate_digest[RTP_DIGEST_LEN];
      char proposal_ref[RTP_PATH_LEN];
      char proposal_origin_hash[RTP_HASH_LEN];
      char diff_ref[RTP_PATH_LEN];
      char diff_origin_hash[RTP_HASH_LEN];
      char chunk_index_ref[RTP_PATH_LEN];
      char repo_root[RTP_PATH_LEN];
      char remote[RTP_PATH_LEN];
      char base_branch[RTP_NAME_LEN];
      char head_branch[RTP_NAME_LEN];
      char workspace_id[RTP_NAME_LEN];
      char workspace_provider[RTP_SHORT_LEN];
      char worktree_path[RTP_PATH_LEN];
      char head_sha[RTP_HASH_LEN];
      char base_sha[RTP_HASH_LEN];
      int proposal_pr_number;
      char proposal_pr_url[RTP_PATH_LEN];
      int impl_pr_number;
      char impl_pr_url[RTP_PATH_LEN];
      char cost_scope[RTP_SHORT_LEN];
      char cost_source[RTP_SHORT_LEN];
      int cost_version;
      double proposal_phase_cost_usd;
      double impl_phase_cost_usd;
      double total_cost_usd;
      int accepted_question_count; /* questions in the brief (for the strict bar) */
      char created_at[RTP_TS_LEN];
      char updated_at[RTP_TS_LEN];
   } rtp_run_t;

   typedef struct
   {
      int id;
      int pipeline_id;
      char phase[RTP_SHORT_LEN];
      char mode[RTP_SHORT_LEN];
      int pass_no;
      char status[RTP_SHORT_LEN];
      char artifact_hash[RTP_HASH_LEN];
      int converged;
      int envelope_valid;
      int blocking_count;
      int suggestion_count;
      int nit_count;
      int open_questions;
      int coverage_gaps;
      int items_round;
      int artifact_round;
      int best_round;
      int rounds_run;
      double cost_usd;
      char result_hash[RTP_HASH_LEN];
      int is_chunked;
      int chunk_total;
      int chunk_done;
      int synthesis_done;
      int chunk_group;       /* >0 groups the chunk-passes of one chunked review */
      int chunk_index;       /* >=0 chunk ordinal; -1 = the whole-artifact synthesis */
      int answered_count;    /* accepted brief questions answered (strict bar) */
      int chunk_offset;      /* this chunk's byte offset into the origin */
      int chunk_len;         /* this chunk's byte length */
      int chunk_omitted;     /* synthesis member: required spans omitted (coverage gap) */
      int chunk_over_budget; /* synthesis member: a span overflowed the budget */
      char created_at[RTP_TS_LEN];
      char updated_at[RTP_TS_LEN];
   } rtp_pass_t;

   /* Aggregate over the passes of one chunked-review group (#28): every member
    * must have a valid completed result and the synthesis member must be done. */
   typedef struct
   {
      int total;       /* chunk members (excludes the synthesis row) */
      int done;        /* chunk members captured/done with a valid envelope */
      int any_invalid; /* a member captured with an invalid envelope */
      int synthesis_present;
      int synthesis_done;
      int blocking_count; /* summed blocking across the group */
      int suggestion_count;
   } rtp_group_agg_t;

   typedef struct
   {
      int id;
      int pass_id;
      int attempt_no;
      char run_id[RTP_RUNID_LEN];
      int is_current;
      char capture_status[RTP_SHORT_LEN];
      char terminal_status[RTP_SHORT_LEN];
      char parse_status[RTP_SHORT_LEN];
      int envelope_valid;
      int items_truncated;
      int truncated;
      int degraded;
      int cost_capped;
      int deadline_hit;
      int cancelled;
      int lost_result;
      char result_hash[RTP_HASH_LEN];
      char result_snapshot[RTP_SNAP_LEN];
      double cost_usd;
      int cost_known;
      char submitted_at[RTP_TS_LEN];
      char terminal_at[RTP_TS_LEN];
   } rtp_attempt_t;

   typedef struct
   {
      int id;
      int pipeline_id;
      int gate_no;
      char verdict[RTP_SHORT_LEN];
      char reason[RTP_REASON_LEN];
      char actor[RTP_NAME_LEN];
      int pr_number;
      char expected_head_sha[RTP_HASH_LEN];
      char merge_sha[RTP_HASH_LEN];
      char merge_executor[RTP_SHORT_LEN];
      char merge_command[RTP_PATH_LEN];
      char merge_output[RTP_SNAP_LEN];
      int merge_exit_code;
      char resolved_at[RTP_TS_LEN];
      char created_at[RTP_TS_LEN];
   } rtp_gate_t;

   /* ---- runs ---- */
   int rtp_run_create(const char *idea, const char *done_bar, const char *repo_root,
                      const char *base_branch, int *out_id);
   int rtp_run_get(int id, rtp_run_t *out);
   /* Full update of all mutable columns from the struct, keyed by out->id. */
   int rtp_run_update(const rtp_run_t *run);
   int rtp_run_set_state(int id, const char *state, const char *phase);
   /* List by state filter (NULL = all non-terminal). Returns count or -1. */
   int rtp_run_list(const char *state_filter, rtp_run_t *out, int max);
   /* Count runs whose admission_class == 'active' (section 1 admission control). */
   int rtp_run_count_active(void);

   /* ---- passes ---- */
   int rtp_pass_create(int pipeline_id, const char *phase, const char *mode, int pass_no,
                       const char *artifact_hash, int *out_id);
   int rtp_pass_get(int id, rtp_pass_t *out);
   int rtp_pass_update(const rtp_pass_t *pass);
   /* Most recent pass for a pipeline+phase (highest pass_no). */
   int rtp_pass_latest(int pipeline_id, const char *phase, rtp_pass_t *out);
   /* Highest pass_no for pipeline+phase, or 0 if none. */
   int rtp_pass_max_no(int pipeline_id, const char *phase);
   /* Highest chunk_group for a pipeline+phase, or 0 if none. */
   int rtp_pass_max_group(int pipeline_id, const char *phase);
   /* Aggregate the passes of one chunked-review group. Returns 0 ok, -1 error. */
   int rtp_pass_group_agg(int pipeline_id, const char *phase, int chunk_group,
                          rtp_group_agg_t *out);

   /* ---- attempts ---- */
   int rtp_attempt_create(int pass_id, int attempt_no, const char *run_id, int *out_id);
   int rtp_attempt_get_by_run(const char *run_id, rtp_attempt_t *out);
   int rtp_attempt_current(int pass_id, rtp_attempt_t *out);
   int rtp_attempt_update(const rtp_attempt_t *attempt);
   /* Highest attempt_no for a pass, or 0 if none. */
   int rtp_attempt_max_no(int pass_id);
   /* Mark every attempt of pass_id except keep_attempt_id as not-current. */
   int rtp_attempt_supersede_others(int pass_id, int keep_attempt_id);

   /* ---- gates ---- */
   int rtp_gate_create(int pipeline_id, int gate_no, int pr_number, const char *expected_head_sha,
                       int *out_id);
   int rtp_gate_get(int pipeline_id, int gate_no, rtp_gate_t *out);
   int rtp_gate_update(const rtp_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_ROUNDTABLE_PIPELINE_H */

/* db1/token_audit.h: per-machine LLM token usage audit log.
 *
 * Every agent/tool turn writes a row capturing prompt/completion/cache
 * tokens and estimated cost. Aggregation happens downstream in
 * dashboard / hud / cmd_core via SQL GROUP BY queries over DB1-owned
 * token_audit + agent_log rows. The write surface is already tier-local;
 * reader cleanup is just an API-shape follow-up, not a storage-boundary
 * blocker.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_TOKEN_AUDIT_H
#define DEC_DB1_TOKEN_AUDIT_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_TOKEN_AUDIT_TOOL_LEN  64
#define DB1_TOKEN_AUDIT_ROLE_LEN  32
#define DB1_TOKEN_AUDIT_MODEL_LEN 64
#define DB1_TOKEN_AUDIT_TS_LEN    32

   typedef struct
   {
      const char *session_id;
      /* When the call was made by a delegate child, the delegation_id of
       * the running delegate (set by the runtime from delegation_active_id).
       * Empty for primary-agent calls. Lets the cost-fold path attribute
       * a child's cost without contaminating the parent's session sum,
       * since session_id is shared between parent and child. */
      const char *delegation_id;
      const char *project_name;
      const char *tool_name;
      const char *role;
      const char *model;
      /* Origin of the turn: where the request entered aimee. "agent" for
       * internal agent/delegate execution; ingress handlers set their own
       * (e.g. "openai-ingress", "anthropic-ingress"). Empty on legacy rows. */
      const char *source;
      /* The model the client asked for, when it differs from the served/billable
       * `model` (e.g. Claude Code requests a model but aimee serves its primary).
       * Empty when not applicable. NULL is treated as empty. */
      const char *requested_model;
      /* Provider stop/finish reason for the turn, when known. NULL == empty. */
      const char *stop_reason;
      /* "realized" (default) for actual provider spend; ingress estimate/dedup
       * paths set "estimated"/"avoided" so readers can exclude them from spend.
       * NULL defaults to "realized". */
      const char *usage_kind;
      /* The agent_log row this audit row belongs to (0 = none, e.g. ingress rows
       * with no agent_log row). Lets agent stats join 1:1 instead of by the lossy
       * (agent_name, role) key. */
      long long agent_log_id;
      /* Idempotency + attribution (proposal §2/#3). request_id is the ingress
       * request id; attempt distinguishes retries of the same request. Together
       * with source they form the idempotency key: a row with a non-empty
       * request_id is inserted at most once per (source, request_id, attempt), so
       * a retried or late-finalized provider call cannot double-count. principal
       * is the account/tenant boundary of the client (e.g. "uid:1000"). All
       * default to empty; empty request_id disables the idempotency guard for
       * that row (internal agent rows). NULL == empty. */
      const char *request_id;
      int attempt;
      const char *principal;
      int prompt_tokens;
      int completion_tokens;
      int cache_write_tokens;
      int cache_read_tokens;
      double estimated_cost_usd;
   } db1_token_audit_row_t;

   /* Insert one audit row. Returns 0 on success, -1 on error. A row carrying a
    * non-empty request_id is inserted with idempotency: a duplicate
    * (source, request_id, attempt) is ignored (still returns 0), never double-counted. */
   int db1_token_audit_insert(const db1_token_audit_row_t *row);

   /* Create the (source, request_id, attempt) idempotency index on the live DB.
    * Idempotent; called once after schema reconcile at init. */
   void db1_token_audit_ensure_idem_index(void);

   /* Sum estimated_cost_usd across rows tagged with this delegation_id.
    * Returns 0.0 for unknown delegation or DB-not-initialized. Used by
    * the cost-fold integration to attribute a child delegate's spend
    * back to its parent's session via db1_cost_fold_record. */
   double db1_token_audit_cost_for_delegation(const char *delegation_id);

   typedef struct
   {
      int total_calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      int64_t cache_write_tokens;
      int64_t cache_read_tokens;
      double estimated_cost_usd;
   } db1_token_audit_totals_t;

   typedef struct
   {
      char role[DB1_TOKEN_AUDIT_ROLE_LEN];
      int calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      double estimated_cost_usd;
   } db1_token_audit_role_summary_t;

   typedef struct
   {
      char tool_name[DB1_TOKEN_AUDIT_TOOL_LEN];
      int calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      double estimated_cost_usd;
   } db1_token_audit_tool_summary_t;

   typedef struct
   {
      char model[DB1_TOKEN_AUDIT_MODEL_LEN];
      int calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      double estimated_cost_usd;
   } db1_token_audit_model_summary_t;

   typedef struct
   {
      char source[64];
      int calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      double estimated_cost_usd;
   } db1_token_audit_source_summary_t;

   typedef struct
   {
      char tool_name[DB1_TOKEN_AUDIT_TOOL_LEN];
      char role[DB1_TOKEN_AUDIT_ROLE_LEN];
      int64_t prompt_tokens;
      int64_t completion_tokens;
      int64_t cache_write_tokens;
      int64_t cache_read_tokens;
      double estimated_cost_usd;
      int call_count;
      char last_seen[DB1_TOKEN_AUDIT_TS_LEN];
   } db1_token_audit_dashboard_row_t;

   /* Aggregate totals, optionally filtered to the last `since_hours`.
    * Pass 0 for all-time. */
   int db1_token_audit_totals(int since_hours, db1_token_audit_totals_t *out);

   /* Realized-vs-estimated-vs-avoided-vs-partial spend breakdown (proposal §7).
    * The single SQL authority for spend semantics: it groups token_audit cost by
    * usage_kind so consumers report realized spend separately from estimated,
    * avoided, and partial rows rather than copy-pasting a WHERE clause.
    * `spend_cost_usd` is the billable total and is REALIZED ONLY — estimated
    * (no provider usage), avoided (dedup-skipped), and partial (aborted) rows are
    * reported individually but are never folded into the billable total. */
   typedef struct
   {
      double realized_cost_usd;  /* provider-reported usage (the billable spend) */
      double estimated_cost_usd; /* no provider usage; aimee-estimated */
      double avoided_cost_usd;   /* dedup-skipped; NOT counted as spend */
      double partial_cost_usd;   /* aborted mid-stream; NOT counted as spend */
      double spend_cost_usd;     /* realized only — excludes estimated/avoided/partial */
   } db1_token_audit_spend_t;

   int db1_token_audit_spend_breakdown(int since_hours, db1_token_audit_spend_t *out);

   /* Per-role aggregates ordered by total prompt+completion tokens DESC.
    * Pass 0 for all-time. */
   int db1_token_audit_by_role(int since_hours, db1_token_audit_role_summary_t *out, int max);

   /* Per-tool aggregates ordered by total prompt+completion tokens DESC.
    * Pass 0 for all-time. */
   int db1_token_audit_by_tool(int since_hours, db1_token_audit_tool_summary_t *out, int max);

   /* Per-model aggregates ordered by total prompt+completion tokens DESC.
    * Pass 0 for all-time. Empty-model rows surface as "(unattributed)". */
   int db1_token_audit_by_model(int since_hours, db1_token_audit_model_summary_t *out, int max);

   /* Per-source (turn origin) aggregates ordered by total tokens DESC. Pass 0
    * for all-time. Empty-source rows surface as "(unattributed)". */
   int db1_token_audit_by_source(int since_hours, db1_token_audit_source_summary_t *out, int max);

   /* Dashboard view grouped by (tool_name, role), ordered by estimated
    * cost DESC, newest activity tiebreaker. */
   int db1_token_audit_list_dashboard(db1_token_audit_dashboard_row_t *out, int max);

   /* === insights extensions === */

   typedef struct
   {
      char platform[32];
      int session_count;
   } db1_insights_platform_t;

   typedef struct
   {
      char session_id[64];
      char title[128];
      char model[64];
      int64_t prompt_tokens;
      int64_t completion_tokens;
      double estimated_cost_usd;
      char created_at[32];
   } db1_insights_top_session_t;

   typedef struct
   {
      char role[32];
      int total;
      int completed;
   } db1_insights_delegate_role_t;

   /* Per-platform (client_type) session counts from server_sessions.
    * Pass 0 for all-time. */
   int db1_insights_by_platform(int since_hours, db1_insights_platform_t *out, int max);

   /* Top sessions by total estimated cost, joining token_audit with
    * server_sessions for title. Pass 0 for all-time. */
   int db1_insights_top_sessions(int since_hours, db1_insights_top_session_t *out, int max);

   /* Per-role delegate counts from delegation_spawns.
    * Pass 0 for all-time. */
   int db1_insights_delegates_by_role(int since_hours, db1_insights_delegate_role_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_TOKEN_AUDIT_H */

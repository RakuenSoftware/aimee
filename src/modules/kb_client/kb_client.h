#ifndef DEC_KB_CLIENT_H
#define DEC_KB_CLIENT_H 1

#include "anti_patterns.h" /* anti_pattern_t */
#include "decision_log.h"  /* db2_decision_log_row_t */
#include "index.h"         /* project_info_t, term_hit_t, blast_radius_t */
#include "memory.h"        /* memory_t, edge_t */
#include "memory_query.h"  /* db2_memory_low_eff_row_t etc. */
#include "rules.h"         /* rule_t */
#include "tasks.h"         /* aimee_task_t */
#include "kb_paths.h"      /* kb_client_default_*_path */
#include <stdint.h>

typedef struct cJSON cJSON;

/* KB health snapshot returned by kb_client_health(). */
typedef struct
{
   int process_ok;          /* aimee-kb is running and responsive */
   char version[128];       /* /v1/version, empty when an older kb omits it */
   int db2_ok;              /* DB2 schema present */
   int db2_kb_tables_ok;    /* kb_documents + kb_async_jobs present */
   int pgvec_ok;            /* pgvector extension loaded in DB2 */
   int pgvec_collection_ok; /* kb_chunks vector table present */
   int pgvec_vectors;       /* vector row count */
   int pgvec_indexed;       /* indexed vector count */
   int embed_ok;            /* embed command configured */
   char embed_command[128];
   int freshness_days; /* days since last ingest; -1 = never */
   char last_ingest_at[64];
   int chunk_count;
   int embedding_count;
   char warnings[512]; /* newline-separated warning strings */
   /* The kb's own verdict on whether it can do its job: "ok" | "degraded".
    * Distinct from process_ok, which only says something answered. Empty when an
    * older kb omits it — callers must treat empty as "no verdict offered", not as
    * ok, or they reintroduce exactly the gap this field closes. */
   char status[16];
   char blockers[512]; /* newline-separated incapacity reasons; empty when ok */
   char last_maintenance_at[64];
   int last_maintenance_rows_decayed;
   int last_maintenance_orphans_pruned;
   int maintenance_enabled;
} kb_health_t;

/* Every retrieval attempt has one of these six outcomes. The accessor is
 * thread-local, so a concurrent request cannot overwrite another request's
 * transport/result classification. Human-readable payloads remain compatible;
 * callers that need to distinguish empty from outage read this typed result. */
typedef enum
{
   KB_CLIENT_RESULT_OK = 0,
   KB_CLIENT_RESULT_EMPTY,
   KB_CLIENT_RESULT_ABSTAINED,
   KB_CLIENT_RESULT_STALE,
   KB_CLIENT_RESULT_UNAVAILABLE,
   KB_CLIENT_RESULT_UNAUTHORIZED
} kb_client_result_status_t;

typedef struct
{
   char state[16]; /* closed | open | half_open */
   unsigned failure_streak;
   unsigned recovery_attempt;
   int64_t retry_after_ms;
   int64_t last_success_ms;
   int64_t last_failure_ms;
   uint64_t suppressed_calls;
} kb_client_dependency_health_t;

kb_client_result_status_t kb_client_last_result_status(void);
const char *kb_client_result_status_name(kb_client_result_status_t status);
int kb_client_result_status_retryable(kb_client_result_status_t status);
/* Heap JSON for the current thread's last result, including dependency,
 * retryability and breaker delay where applicable. Caller frees. */
char *kb_client_last_result_json(const char *message);
void kb_client_dependency_health(kb_client_dependency_health_t *out);

/* Deterministic test seams; production passes NULL and uses the wall clock. */
void kb_client_dependency_reset_for_tests(void);
/* Clear stale transport failure budgets after an operator has installed or
 * replaced the durable KB identity. Safe to call while requests are active. */
void kb_client_dependency_reset(void);
void kb_client_dependency_set_clock_for_tests(int64_t (*now_ms)(void));

/* Query aimee-kb health.  Fills *out and returns 0 on success.  Returns -1
 * if aimee-kb is unreachable (out->process_ok == 0). */
int kb_client_health(kb_health_t *out);

/* Returns the raw /v1/health JSON document (caller frees), or NULL if aimee-kb
 * is unreachable or returns non-2xx. Used by `aimee kb curator status` to read
 * the curator block (richer than the flat kb_health_t snapshot). */
char *kb_client_health_json(void);

/* Fetch the KB's agent-facing one-surface capability projection from
 * /v1/capabilities. Returns a heap JSON object with cli_only and mcp_only
 * arrays, or NULL when the KB is unreachable or does not advertise the
 * projection. The runtime merges this into its client-facing capabilities. */
char *kb_client_agent_surfaces_json(void);

/* kb_client_typed_facts_enabled() is retired: the typed-fact layer is
 * unconditional, so aimee-server has nothing to ask the KB about. */
/* §2c: POST /v1/reembed; raw response JSON (caller frees) or NULL on transport
 * failure; *status_out (optional) gets the HTTP status. target_dim>0 pins the
 * reset target (bypasses the embedder probe); clear_maintenance!=0 instead just
 * force-clears a stuck reembed_in_progress marker (ignores confirm/force/dim). */
char *kb_client_reembed(int confirm, int force, int dry_run, int target_dim, int clear_maintenance,
                        int *status_out);
char *kb_client_subject_erasure_begin(const char *request_id, const char *subject,
                                      cJSON *session_ids, int *status_out);
char *kb_client_subject_erasure_complete(const char *request_id, int64_t db1_count,
                                         int *status_out);

/* Returns the curator observability block (§4) from aimee-kb's /v1/health as a
 * standalone heap JSON object (caller frees). Backs the server's GET
 * /v1/kb/curator surface. Never NULL — a status-error object on failure. */
char *kb_client_curator_json(void);

/* Returns a heap-allocated JSON document describing kb/vector status.
 * Caller must free the returned string. Never returns NULL. */
char *kb_client_status_json(void);

/* Per-project KB status via the public /v1/health status mode with the
 * given project and returns the heap-allocated JSON response
 * (caller frees).  On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
char *kb_client_project_status_json(const char *project);

/* Returns a heap-allocated JSON document for the vector-status subsection
 * of kb_client_status_json(). Caller must free the returned string. */
char *kb_client_vector_status_json(void);

/* Run a KB search via aimee-kb's public /v1/search endpoint with the given
 * project / query / embedding_command / max_results / format ("text" or
 * "json") and returns the heap-allocated JSON response envelope (caller
 * frees).  The result payload lives at
 * resp["result"] and is either the plain-text or JSON search output
 * depending on `format`.  On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
/* Search KB documents via aimee-kb.  |project| may be NULL or empty only for
 * an intentional all-project internal call; the client sends scope=all
 * explicitly. Agent-facing surfaces resolve the active project before here.
 * |format| selects "text" (default) or "json"
 * formatting of the result body.  Returns a heap-allocated JSON
 * envelope (caller frees). */
char *kb_client_search_json(const char *project, const char *query, const char *embedding_command,
                            int max_results, const char *format);

/* Like kb_client_search_json but with a per-call fusion mode override.
 * fusion_mode_override: "rrf" | "static_alpha" | "dynamic_alpha"; NULL = use server config. */
char *kb_client_curator_implements_json(const char *topic);
char *kb_client_curator_synthesize_json(const char *topic);
char *kb_client_curator_contradictions_json(int limit);
char *kb_client_search_json_ex(const char *project, const char *query,
                               const char *embedding_command, int max_results, const char *format,
                               const char *fusion_mode_override);
/* Explicit-scope variant. When all_projects is true, preferred_project is the
 * active-project head bucket and the remaining corpus may extend the tail. */
char *kb_client_search_json_scoped_ex(const char *preferred_project, int all_projects,
                                      const char *query, const char *embedding_command,
                                      int max_results, const char *format,
                                      const char *fusion_mode_override);

/* Run a KB build via aimee-kb's public /v1/code/build endpoint and returns
 * the heap-allocated JSON response (caller frees).  Long-running for large
 * repos; uses the same timeout as /v1/maintenance/repair.  On any failure
 * the returned JSON has {"status":"error","message":"..."}. */
char *kb_client_build_json(const char *path, const char *project, const char *embedding_command,
                           int force);

/* --- Phase 5: Code embedding refresh RPC (server -> KB boundary) --- */

typedef struct
{
   int accepted;
   char job_id[128];
   char project[256];
   char scope[64];
   int64_t estimated_points;
   int64_t skipped_unchanged;
   int64_t embedded;
   int dry_run;
   char writer[64];
} kb_client_code_embed_result_t;

/* Request code embedding refresh from aimee-kb.
 * project: project name; scope: "changed_files"|"full_project".
 * paths/path_count: optional path filter (NULL/0 = all in scope).
 * batch_size/max_points: caps (0 = server defaults).
 * dry_run: 1 = report without writing.
 * Returns 0 on success (result filled), -1 if kb unreachable. */
int kb_client_code_embeddings_refresh(const char *project, const char *scope, const char **paths,
                                      int path_count, int batch_size, int max_points, int dry_run,
                                      kb_client_code_embed_result_t *out);

/* Search code embeddings via aimee-kb.  Returns count of hits. */

/* --- Graph code projection / explain (server -> KB boundary) --- */

/* Result of `graph sync-code`: the published generation and its edge count. */
typedef struct
{
   int ok;
   int64_t generation_id;
   int64_t edge_count;
   char project[256];
} kb_client_graph_sync_result_t;

/* Project the code index into entity_edges via aimee-kb (graph.sync_code).
 * Returns 0 on success (out filled), -1 if kb unreachable / sync failed. */
int kb_client_graph_sync_code(const char *project, kb_client_graph_sync_result_t *out);

/* Fetch a heap-allocated JSON explain payload for |entity| via aimee-kb
 * (graph.explain): incident edges with relation, weight, structural_weight,
 * utility_score, edge_origin, plus canonical node metadata when present.
 * Caller frees.  Returns NULL if kb is unreachable. */
char *kb_client_graph_explain_json(const char *entity, int limit);

/* Run an incremental KB update via aimee-kb's public /v1/code/update endpoint
 * and returns the heap-allocated JSON response (caller frees).  On any
 * failure the returned JSON has {"status":"error","message":"..."}. */
char *kb_client_update_json(const char *path, const char *project, const char *embedding_command);

/* Ingest all projects in a workspace (or all configured workspaces when
 * workspace is NULL/empty) via aimee-kb's public /v1/ingest endpoint and
 * returns the heap-allocated JSON response (caller frees).  Long-running for
 * large workspaces.  On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
char *kb_client_ingest_json(const char *workspace, const char *embedding_command, int force);
char *kb_client_docs_manifest_json(const char *scope, const char **paths, int path_count);
char *kb_client_docs_upload_json(const char *scope, const char **paths, int path_count);
char *kb_client_docs_push_json(const char *scope, const char **paths, int path_count);
/* Thin-client variant: the caller has already read each document on the client
 * host, so the server never tries to dereference a path it cannot see. */
char *kb_client_docs_push_content_json(const char *scope, const char **doc_keys,
                                       const char **contents, const int *content_lengths,
                                       int doc_count);

/* Query the background ingest queue (kb_ingest_queue) state and recent completions.
 * Returns heap-allocated JSON with queue stats, active workers, and last 10 jobs. */
char *kb_client_ingest_status_json(void);

/* Return heap-allocated JSON describing aimee-kb's connection worker slots. */
char *kb_client_workers_json(void);

/* Query the async embedding queue (kb_async_jobs) via aimee-kb's public
 * /v1/pipeline/status endpoint and returns the heap-allocated JSON response
 * (caller frees). On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
char *kb_client_queue_status_json(void);
char *kb_client_corpus_pipeline_status_json(void);

/* Fetch a single kb_async_jobs row via aimee-kb's public /v1/jobs/{id}
 * endpoint and returns the heap-allocated JSON response (caller frees). */
char *kb_client_job_status_json(int64_t job_id);

/* Drain the async embedding queue via aimee-kb's public /v1/drain endpoint
 * with {embedding_command, timeout} and returns the heap-allocated JSON
 * response (caller frees). On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
char *kb_client_queue_drain_json(const char *embedding_command, int timeout_secs);
char *kb_client_corpus_pipeline_drain_json(int limit);

/* Reconcile vector points against the DB2 source of truth: prune
 * points whose memory/kb_document rows no longer exist. Calls the public
 * /v1/maintenance/reconcile endpoint with {dry_run} and returns the
 * heap-allocated JSON
 * response (caller frees).  On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
char *kb_client_reconcile_json(int dry_run);

/* Create a memory directive via the aimee-kb sidecar.  Sends
 * `memory.directive_create` with the usual field set and returns
 * {"status":"ok","dedup":0|1,"directive":{...}} on success.  dedup=1
 * means the server rejected as duplicate of an existing row (no
 * directive object returned).  Caller frees. */

/* Transition an open directive to resolved.  Sends
 * `memory.directive_resolve` with {id, with_memory, note}.  Caller frees. */

/* Mark an open directive as suppressed.  Sends
 * `memory.directive_suppress` with {id}.  Caller frees. */

/* Sweep expired directives.  Returns {"status":"ok","expired":N}.  Caller
 * frees. */

/* List directives filtered by state/cause with a cap of `limit` rows.
 * Returns {"status":"ok","directives":[...]} on success.  Caller frees. */

/* Fetch curiosity items via the aimee-kb sidecar. Sends `curiosity.list`
 * with {state, limit} and returns the heap-allocated JSON response
 * (caller frees). The response has {"status":"ok","items":[...]} where
 * each element is the curiosity_item_t shape. On any failure the
 * response has {"status":"error","message":"..."}. */
char *kb_client_curiosity_list_json(const char *state, int limit);

/* Curiosity backlog write/read RPCs forwarded to aimee-kb. Each
 * returns a heap-allocated JSON response (caller frees) of shape
 * {"status":"ok",...} on success, or
 * {"status":"error","message":"..."} on failure. */
char *kb_client_curiosity_create_json(const char *gap_type, const char *target_entity,
                                      const char *target_topic, const char *evidence,
                                      double importance, double novelty,
                                      const char *source_session);
char *kb_client_curiosity_sweep_json(void);
char *kb_client_curiosity_rescore_json(void);
char *kb_client_curiosity_get_json(int64_t id);
char *kb_client_curiosity_update_state_json(int64_t id, const char *new_state);

/* Pull the top-N open curiosity items, route each into an epistemic
 * directive, and transition the source items to in_progress (idempotent
 * on re-run via the directive unique index). Returns
 * {"status":"ok","routed":N}. Caller frees. */
char *kb_client_curiosity_route_top_json(int limit, const char *source_session);

/* Investigation-note RPCs forwarded to aimee-kb. Each returns a
 * heap-allocated JSON response (caller frees) of shape
 * {"status":"ok",...} on success, or
 * {"status":"error","message":"..."} on failure. */
char *kb_client_note_create_json(const char *title, const char *content, const char *tags,
                                 const char *author);
char *kb_client_note_list_json(const char *tag, int limit);
char *kb_client_note_search_json(const char *query, int limit);

/* Spec-driven roadmap RPCs via the aimee-kb sidecar (DB2 artifacts). Each
 * returns the heap-allocated JSON response string (caller frees), or NULL on a
 * transport failure. Roadmap data lives in DB2, owned by aimee-kb; the CLI and
 * server route every roadmap op through these wrappers and never touch DB2
 * directly. Defined in src/modules/kb_client/kb_client_roadmap.c. */
/* Sends `roadmap.create_from_decomposition` with {decomposition} (the JSON
 * decomposition document). Response: {"status":"ok","roadmap_id":"..."}. */
char *kb_client_roadmap_create_json(const char *decomposition_json);
/* Sends `roadmap.validate` with {decomposition}. Response:
 * {"status":"ok","valid":true|false,"reason":"..."}. */
char *kb_client_roadmap_validate_json(const char *decomposition_json);
/* Sends `roadmap.show` with {roadmap_id}. Response:
 * {"status":"ok","roadmap":{...},"units":[...]}. */
char *kb_client_roadmap_show_json(const char *roadmap_id);
/* Sends `roadmap.list`. Response: {"status":"ok","roadmaps":[...]}. */
char *kb_client_roadmap_list_json(void);
/* Sends `roadmap.rebuild` with {roadmap_id}. Response: {"status":"ok"}. */
char *kb_client_roadmap_rebuild_json(const char *roadmap_id);
/* Sends `roadmap.report` with {roadmap_id, output_path?}. Generates a
 * self-contained HTML report. Response: {"status":"ok","report_path":"..."}. */
char *kb_client_roadmap_report_json(const char *roadmap_id, const char *output_path);

/* Fetch rules via the aimee-kb sidecar.  Sends `rules.list` with
 * {limit} and returns the heap-allocated JSON response (caller frees)
 * of shape {"status":"ok","rules":[...]}.  Sends `rules.generate` and
 * returns {"status":"ok","content":"..."} (a markdown rules.md
 * blob).  Failures come back as {"status":"error","message":"..."}. */
char *kb_client_rules_list_json(int limit);
char *kb_client_rules_generate_json(void);

/* Convenience: extract the markdown body from rules.generate's
 * envelope.  Returns a heap-allocated string (caller frees) or NULL
 * if kb is unreachable / no rules configured.  Mirrors
 * db2_rules_generate(). */
char *kb_client_rules_generate(void);

/* List rules via aimee-kb into a rule_t buffer.  Mirrors
 * db2_rules_list() in shape so daemon-side / CLI-fork callers can
 * swap the direct call for this RPC without changing downstream
 * code.  Returns the number of rows written into |out| (0 if kb is
 * unreachable). */
int kb_client_rules_list(rule_t *out, int max_rules);

/* Propose a collaborative rule via aimee-kb (the DB2 owner).  Returns
 * the new rule id (>=0) or -1 if kb is unreachable / proposal
 * rejected.  Mirrors db2_collab_rules_propose(). */
int kb_client_collab_rules_propose(const char *text, const char *reason, const char *proposed_by);

/* List collaborative-rule proposals via aimee-kb.  Returns a heap-
 * allocated JSON array string (caller frees) or NULL if kb is
 * unreachable.  Same shape as db2_collab_rules_json_all(). */
char *kb_client_collab_rules_list_json(void);

/* List active collab rules + epoch via aimee-kb.  Returns a heap-
 * allocated JSON object string (caller frees) of shape
 * {"epoch":N,"rules":[...]} or NULL if kb is unreachable.  Mirrors
 * db2_collab_rules_json_active(). */
char *kb_client_collab_rules_list_active_json(void);

/* Approve / reject / retire a collab rule by id via aimee-kb.  Return 0
 * on success, -1 if kb is unreachable or the action failed.  Mirror
 * db2_collab_rules_approve / _reject / _retire. */
int kb_client_collab_rules_approve(int rule_id);
int kb_client_collab_rules_reject(int rule_id);
int kb_client_collab_rules_retire(int rule_id);

/* Render the collab-rules system-prompt section via aimee-kb.  Returns
 * a heap-allocated string (caller frees) or NULL when the agent epoch
 * matches and nothing needs reinjecting, or kb is unreachable.
 * Mirrors db2_collab_rules_inject(). */

struct cJSON;

/* Run the learning-router record-signal pipeline via aimee-kb.
 * |args| is the MCP tool args object (signal_type + optional
 * descriptive fields + optional evidence_refs array).  Returns the
 * stringified envelope (caller frees) or NULL if kb is unreachable. */
char *kb_client_learning_propose_signal_json(const struct cJSON *args);

/* Record a per-agent run outcome via aimee-kb.  Mirrors
 * db2_agent_outcome_record().  Returns 0 on success, -1 if kb is
 * unreachable. */
int kb_client_agent_outcome_record(const char *agent_name, const char *role,
                                   const char *outcome_kind, const char *reason, int turns_used,
                                   int tools_called, int64_t tokens_used,
                                   const char *tool_error_pattern);

/* Find-and-consume the highest-priority queued hint for (role, prompt)
 * via aimee-kb.  Returns a heap-allocated string (caller frees) or
 * NULL if no hint matches / kb is unreachable.  Mirrors
 * db2_agent_hint_find_and_consume(). */
char *kb_client_agent_hint_consume(const char *role, const char *prompt);

/* Run the anti-pattern maintenance routines from memory_advanced.c
 * inside aimee-kb (the DB2 owner).  Each returns the count of
 * entries acted on, or -1 if kb is unreachable. */
int kb_client_anti_pattern_extract_from_feedback(void);
int kb_client_anti_pattern_extract_from_failures(void);
int kb_client_anti_pattern_escalate(int hit_threshold);

/* Session-wrapup maintenance routines that touch DB2 — wrapped on
 * the kb side so daemon and CLI-fork callers actually execute them.
 * Each returns the count of rows adjusted/generated/decayed, or -1
 * if kb is unreachable.
 *
 * eval_feedback_loop is intentionally NOT in this set: it combines DB1
 * eval-task inputs with DB2 rule-weight writes and must be split across
 * the server/kb boundary before it can become a pure kb RPC. */
int kb_client_rules_decay(void);
int kb_client_memory_learn_style(void);

/* Decision-log + anti-pattern CRUD via aimee-kb (the DB2 owner).
 * Each mirrors the local db2_* signature; row-out variants populate
 * |out| if non-NULL.  Reads return number of rows written; writes
 * return 0 on success, -1 on failure / kb-unreachable. */
int kb_client_decision_log_insert(int64_t task_id, const char *options, const char *chosen,
                                  const char *rationale, const char *assumptions,
                                  db2_decision_log_row_t *out);
int kb_client_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out,
                                int max);
int kb_client_anti_pattern_list(anti_pattern_t *out, int max);
int kb_client_anti_pattern_insert(const char *pattern, const char *description, const char *source,
                                  const char *source_ref, double confidence, anti_pattern_t *out);
int kb_client_anti_pattern_delete(int64_t id);

/* Match (file_path, command) against stored anti-patterns via
 * aimee-kb.  Returns the number of rows written into |out| (0 if
 * kb is unreachable / no match).  Mirrors db2_anti_pattern_check(). */
int kb_client_anti_pattern_check(const char *file_path, const char *command, anti_pattern_t *out,
                                 int max);

/* Increment the lifetime hit_count for an anti-pattern via
 * aimee-kb.  Returns 0 on success, -1 if kb is unreachable.  Mirrors
 * db2_anti_pattern_bump(). */
int kb_client_anti_pattern_bump(int64_t id);

/* Rules + feedback CRUD via aimee-kb (the DB2 owner).  Mirrors the
 * local db2_* signatures.  rules.delete / update_directive_type
 * return 0 on success, -1 on failure; feedback.record returns the
 * new row id (>=0) or -1.  reinforced_out (optional) is set to 1
 * iff the row was an existing reinforcement rather than a fresh
 * insert. */
int kb_client_rules_delete(int id);
int kb_client_rules_update_directive_type(int id, const char *directive_type);
int kb_client_feedback_record(const char *polarity, const char *title, const char *description,
                              int weight, int *reinforced_out);

/* Session-wrapup maintenance routines that also touch DB2 — wrapped
 * on the kb side.  expire_session_directives returns 0 on success
 * / -1 on kb-unreachable; scan_conversations returns the # of items
 * processed (-1 on failure). */
int kb_client_directive_expire_session(void);
int kb_client_memory_scan_conversations(char dirs[][4096], int dir_count);

/* Cross-tier maintenance is not a kb-only RPC. Server-side ports must
 * perform DB1 reads inside aimee-server and send only DB2 writes or
 * lookups through kb_client/aimee-kb. Do not add an external process between
 * the two DB-owning daemons. */

/* Dashboard endpoints whose body is composed inside aimee-kb (which
 * has DB2 open).  Each returns the raw JSON body that the equivalent
 * api_* function would have produced (caller frees), or NULL if kb
 * is unreachable. */
char *kb_client_dashboard_memory_stats_json(void);
char *kb_client_dashboard_logs_json(void);
char *kb_client_dashboard_reminders_json(void);
char *kb_client_dashboard_recall_json(void);
char *kb_client_dashboard_directives_json(void);

/* Render the "Open Commitments" / "Unresolved Questions" briefing
 * sections via aimee-kb.  Returns a heap-allocated markdown fragment
 * (caller frees) or NULL when the section is empty / kb is unreachable.
 * Mirror session_briefing_render_commitments / _directives. */
char *kb_client_session_briefing_commitments(int limit);
char *kb_client_session_briefing_directives(int limit);

/* Run memory maintenance (replay/compact/prune/summarize) inside aimee-kb.
 * Returns the kb response envelope as JSON (caller frees) including the
 * summary object.  Mirrors memory_maintenance_run(). */

/* Session recall via aimee-kb. Returns the response envelope as JSON
 * (caller frees), with the bundle nested under "recall". */
char *kb_client_memory_recall_json(const char *task_hint, int limit_tokens, int session_start);
/* Shared-store recall without the legacy personal-memory merge. */
char *kb_client_memory_recall_shared_json(const char *task_hint, int limit_tokens,
                                          int session_start);
/* ABI-compatible legacy form; the fusion argument is ignored. The receiving
 * instance applies its own configuration. */
char *kb_client_memory_recall_json_ex(const char *task_hint, int limit_tokens, int session_start,
                                      const char *graph_code_fusion_state);

/* Audit hook: notified after each SERVER-INITIATED memory mutation via aimee-kb
 * (insert / update / delete / reject) with NON-CONTENT fields only — the
 * operation, the memory id, and (for insert) the tier / kind / key identity,
 * confidence, and session — plus whether the kb call succeeded. The memory
 * CONTENT (and use_cases / reject reason), the PII-bearing payload, is NEVER
 * passed. This is the SERVER's view of the memory changes it requested; aimee-kb
 * records the authoritative event on its own bus at the mutation site. Installed
 * once at startup by a server-only bridge forwarding to the observability bus;
 * kb_client itself has NO event-bus dependency (stays linkable everywhere). NULL
 * by default. Set once before serving. */
typedef void (*kb_client_memory_audit_hook_fn)(const char *op, int64_t id, const char *tier,
                                               const char *kind, const char *key, double confidence,
                                               const char *session_id, int ok);
void kb_client_set_memory_audit_hook(kb_client_memory_audit_hook_fn fn);

/* Internal: fire the memory-audit hook (if installed). Defined in the dep-free
 * kb_client_memory_audit.c (kept free of RPC/cJSON so the bridge->bus test can
 * link the seam without the whole kb_client stack); callable from the kb_client
 * memory TUs (mutations, and kb_client_memory.c for delete). */
void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session_id,
                                 int ok);

/* Reject a memory: preserve it as reviewable history, remove it from recall,
 * and install an exact-value tombstone that blocks automatic re-extraction.
 * Optional reason is retained with the row and mutation evidence.
 * Returns 0 on success, -1 on failure / kb unreachable. */
int kb_client_memory_reject(int64_t id, const char *reason);
/* Compact conversation windows (raw->summary, summary->fact) via aimee-kb.
 * Returns 0 / -1.  Mirrors memory_compact_windows(). */
int kb_client_memory_compact_windows(int *summary_count, int *fact_count);

/* Assemble the agent context block via aimee-kb.  Returns a heap-allocated
 * markdown string (caller frees) or NULL on failure / kb unreachable.
 * Mirrors memory_assemble_context(). */
char *kb_client_memory_assemble_context(const char *task_hint);

/* Assemble the default temporal-learning context (current semantic assertions,
 * active observations, and reviewed procedures) via aimee-kb. Returns the
 * trust-labelled rendered context, or NULL when unavailable or empty. */
char *kb_client_memory_assemble_typed_context(const char *query);

/* Export rules to JSONL via aimee-kb.  Returns row count or -1. */
int kb_client_rules_export_jsonl(const char *path);

/* Insert a rule via aimee-kb.  Returns 0 on success, -1 on failure /
 * kb unreachable.  Mirrors db2_rules_insert(). */
int kb_client_rules_insert(const char *polarity, const char *title, const char *description,
                           int weight);

/* Look up a single tool registry entry via aimee-kb.  Returns 0 on
 * success (out_found set to 0/1), -1 on failure / kb unreachable.
 * Mirrors db2_tool_registry_lookup(). */
int kb_client_tool_registry_lookup(const char *name, char *out_input_schema, size_t schema_cap,
                                   char *out_side_effect, size_t se_cap, int *out_enabled,
                                   int *out_found);

/* Snapshot the full tool_registry table for caching.  Returns the
 * heap-allocated JSON envelope (caller frees) or NULL when kb is
 * unreachable.  Used by aimee-server to populate a process-local
 * cache so per-tool-call validation does not RPC every turn. */
char *kb_client_tool_registry_snapshot_json(void);

/* Invoke a tool on an MCP plugin the KB HOSTS (config install: kb) over the mTLS
 * /v1/actions/mcp.call channel.  |qualified_name| is "<client>:<tool>".  |actor|
 * identifies the caller (the dispatch role) for the kb's content-free audit row;
 * may be NULL.  On success returns 0 and, when out_result is non-NULL, sets it to
 * an owned cJSON (the plugin's result); on failure returns -1 with a message in
 * err_buf. |args| is borrowed. */
int kb_client_mcp_call(const char *qualified_name, const cJSON *args, int timeout_ms,
                       const char *actor, cJSON **out_result, char *err_buf, size_t err_buf_len);

/* List the memory_relation_schema rows owned by aimee-kb.  Writes up
 * to |max| rows into |out| and returns the number written. */

/* Diagnose a query (returns memory_t + memory_score_parts_t per row)
 * via aimee-kb.  Returns row count.  Mirrors memory_diagnose() and
 * memory_diagnose_scoped(). */
int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max);
int kb_client_memory_diagnose_scoped(const char *query, const char *scope_type,
                                     const char *scope_value, int limit, memory_diagnostic_t *out,
                                     int max);

/* Explain how a specific memory matches a query via aimee-kb.
 * Returns 0 / -1.  Mirrors memory_explain_match(). */
int kb_client_memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out);

/* Fetch a single memory row by id via aimee-kb. Returns 0 on success,
 * 1 for a valid missing row, or -1 when the service/result is unavailable. */
int kb_client_memory_get(int64_t id, memory_t *out);

/* The EVENT-time verdict for an `as_of` query, kept as a tri-state because the
 * three answers are genuinely different. UNKNOWN is the service saying "I could
 * not tell"; folding it into NO is how a bitemporal query lies. UNASKED is the
 * ordinary no-as_of fetch, which must emit no verdict at all rather than a
 * default one. */
typedef enum
{
   KB_VALID_AT_UNASKED = 0,
   KB_VALID_AT_YES,
   KB_VALID_AT_NO,
   KB_VALID_AT_UNKNOWN
} kb_valid_at_t;

/* Read-by-id JSON variant for lossless presentation/audit surfaces. The
 * returned memory object belongs to the caller. Unlike memory_t, its content
 * is not capped at 2047 bytes. Returns 0 on hit, 1 on miss, -1 on failure. */
int kb_client_memory_get_json_as_of(int64_t id, const char *as_of, cJSON **out,
                                    kb_valid_at_t *verdict);

/* As-of variant of kb_client_memory_get: forwards `as_of` to aimee-kb, which
 * owns the valid_from/valid_until interval, and hands back its verdict. The
 * plain entry point above cannot express this -- memory_t has no field for it
 * (the valid_at in memory.h belongs to memory_relation_t, a different struct),
 * so a caller that wants the time answer has to receive it separately.
 * `as_of` NULL or empty asks nothing and leaves *verdict at UNASKED. */
int kb_client_memory_get_as_of(int64_t id, const char *as_of, memory_t *out,
                               kb_valid_at_t *verdict);

/* Insert a memory row via aimee-kb (the DB2 owner).  The full
 * write-side gate pipeline runs inside aimee-kb.  Returns 0 on
 * success (|out| filled if non-NULL) or -1 if kb is unreachable or
 * the gate rejected the write.  Mirrors memory_insert(). */
/* Returned when memory content carries a secret/PII span that cannot be cleanly
 * redacted. The write is REFUSED CLIENT-SIDE: no request is issued, so the text
 * never reaches aimee-kb. Distinct from -1 (kb unreachable or kb-side rejection)
 * so a caller can tell "we would not send this" from "we could not send this".
 * Every content-carrying memory.* wrapper below returns it. */
#define KB_CLIENT_MEMORY_WITHHELD_PII (-2)

/* The same refusal from any other content-carrying wrapper: rules, notes,
 * decisions, anti-patterns, feedback, tasks, directives, curiosity gaps and
 * prospective memories all persist session-authored prose into aimee-kb and
 * are screened by the same client-side gate. Wrappers that return a response
 * document instead of a status code return kb_client_pii_withheld_json()'s
 * error envelope rather than this code. */
#define KB_CLIENT_WITHHELD_PII (-2)

int kb_client_memory_insert(const char *tier, const char *kind, const char *key,
                            const char *content, double confidence, const char *session_id,
                            memory_t *out);
int kb_client_memory_insert_ex(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_t *out);

/* Same, but says whose words these are. The authority is recorded as the row's
 * provenance and decides whether the typed-fact drain may later mint Class-A
 * facts from this note (memory.h, memory_insert_ex). MEMORY_AUTHORITY_USER is
 * for a surface where the USER is the author — the `memory store` CLI, the
 * onboarding wizard — never for text the agent composed, and the kb still checks
 * that the request authenticated as a person before honouring it. The two
 * spellings above are the MODEL-authority ones. */
int kb_client_memory_insert_as(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_authority_t authority, memory_t *out);

/* Look up a memory id by (key, kind) via aimee-kb.  Returns 0 if no
 * row matches or kb is unreachable; the row id otherwise.  Mirrors
 * db2_memory_find_id_by_key_kind(). */
int64_t kb_client_memory_find_id_by_key_kind(const char *key, const char *kind);

/* Supersede an existing memory with a new content via aimee-kb (the
 * DB2 owner).  The kb side runs the full version-bump + provenance +
 * link pipeline.  Returns 0 on success (out filled if non-NULL) or
 * -1 if kb is unreachable / supersede failed.  Mirrors
 * the shared Go replacement command. */

/* Set the artifact_type / artifact_ref / artifact_hash columns on a
 * memory row via aimee-kb.  Returns 0 on success, -1 on failure /
 * missing row.  Mirrors db2_memory_set_artifact(). */

/* Session-scope priority memory listings via aimee-kb.  Used by the
 * session-start prompt builder to populate # Project / # Workspace /
 * # Global Context sections.  Each returns the number of rows
 * written into |out| (0 if kb is unreachable).  Mirrors
 * the Go memory.list_session_scope_priority commands. */

/* Run the active-task drift check via aimee-kb.  Returns 0 on
 * success (|out| filled) or -1 if kb is unreachable / task missing.
 * Mirrors memory_check_drift(); see headers/memory.h for
 * drift_result_t. */
int kb_client_memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                                 drift_result_t *out);

/* Search facts/patterns by free-text keyword via aimee-kb.  Returns
 * the number of rows written into |out| (0 if kb is unreachable).
 * Mirrors db2_memory_search_facts_patterns_by_keyword(). */

/* Task CRUD via aimee-kb (the DB2 owner).  Each mirrors the local
 * db2_task_* signature.  See db2/tasks.h for aimee_task_t /
 * task_edge_t. */
int kb_client_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out,
                        int max);
int kb_client_task_create(const char *title, const char *session_id, int64_t parent_id,
                          aimee_task_t *out);
int kb_client_task_update_state(int64_t id, const char *state);
int kb_client_task_delete(int64_t id);
int kb_client_task_add_edge(int64_t source, int64_t target, const char *relation);
int kb_client_task_get_edges(int64_t task_id, task_edge_t *out, int max);

/* Auditable-correctness P1: ask the KB to record a single per-turn
 * retrieval_event keyed by `turn_id` (a UUID), listing the int64 memory row ids
 * surfaced into the turn. `role` is the recall op (e.g. "Recall"),
 * `query_fingerprint` identifies the turn query. Returns 0 on success (event
 * written), -1 on bad args or kb error. The KB write uses the already-merged
 * db2_demotion_retrieval_event_write_turn (first-wins on duplicate turn_id). */
int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids);

/* Like the above, but also returns the minted retrieval_event_id in event_id_out
 * (so a caller can later attribute outcomes to it). */
int kb_client_evidence_emit_retrieval_event_ex(const char *turn_id, const char *role,
                                               const char *query_fingerprint, const int64_t *ids,
                                               int n_ids, char *event_id_out, size_t event_id_len);

/* Record retrieval outcomes for surfaced rows against an event.
 * surface: "memory" -> retrieval_attribution, "ranker" -> ranker_outcome. */
int kb_client_record_retrieval_outcome(const char *surface, const char *event_id,
                                       const int64_t *ids, int n, const char *verdict);

/* Mint a kb_hybrid retrieval_event over the surfaced doc_ids (ranker.emit_event)
 * and return its id in event_id_out. Used by the kb_search tool-capture to give
 * the outcome bridge an event to attribute against. 0 on success, -1 on error. */
int kb_client_ranker_emit_event(const int64_t *doc_ids, int n, const char *query_fingerprint,
                                char *event_id_out, size_t event_id_len);

/* Auditable-correctness P1.5: merge typed code/doc refs into the turn's event via
 * the KB evidence.merge_retrieval_event action (the idempotent two-writer upsert).
 * `types`/`refs`/`versions` are parallel arrays of length `n` (entries with an
 * empty type or ref are skipped; `versions` may be NULL). Returns 0 on success,
 * -1 on bad args or kb error. */
int kb_client_evidence_merge_retrieval_event(const char *turn_id, const char *role,
                                             const char *query_fingerprint,
                                             const char *const *types, const char *const *refs,
                                             const char *const *versions, int n);

/* Auditable-correctness P1: the /v1/audit/trace read — forward to the KB
 * evidence.trace_retrieval_event action and return its JSON response verbatim
 * (malloc'd, caller frees; NULL on bad arg or kb error). */
char *kb_client_evidence_trace_retrieval_event(const char *turn_id);

/* Auditable-correctness P2: the /v1/audit/provenance read — forward to the KB
 * evidence.provenance_retrieval_event action and return its JSON response
 * verbatim (malloc'd, caller frees; NULL on bad arg or kb error). */
char *kb_client_evidence_provenance_retrieval_event(const char *turn_id);

/* Auditable-correctness P3: the /v1/audit/fidelity read — forward to the KB
 * evidence.fidelity_retrieval_event action and return its JSON response verbatim
 * (malloc'd, caller frees; NULL on bad arg or kb error). */
char *kb_client_evidence_fidelity_retrieval_event(const char *turn_id);

/* Invoke an action through the authenticated KB transport. Takes ownership of
 * req; the caller frees the returned JSON, including non-success envelopes. */
char *kb_v1_action_request(const char *action, cJSON *req);
/* Same ownership and authentication, with a caller-selected operation budget. */
char *kb_v1_action_request_with_timeout(const char *action, cJSON *req, int timeout_ms);

/* Fetch learning proposals via the aimee-kb sidecar.  Sends
 * `learning.list_proposals` with {state, sink, limit} and returns the
 * heap-allocated JSON response (caller frees).  The response has
 * {"status":"ok","proposals":[...]} where each element is the
 * `learning_proposal_to_json` shape.  On any failure the response has
 * {"status":"error","message":"..."}. */
char *kb_client_learning_list_proposals_json(const char *state, const char *sink, int limit);

/* The endogeneity gate, answered by the knowledge service because the ledger it
 * reads is DB2 and the daemon builds without it. Returns the response JSON (the
 * caller frees), or NULL when the service is unreachable — which is NOT the same
 * as a closed gate, and callers must not conflate them. */
char *kb_client_learning_endogeneity_json(int window_days);

/* Record what became of a committed proposal (S5). The router observes
 * supersession and post-commit rejection itself; this is how a judgement it
 * cannot make — that a commit was CONTRADICTED — gets entered. */
char *kb_client_learning_fate_json(int id, const char *fate, const char *reason);

/* Drain the curiosity backlog (S4). Served by the knowledge service: the
 * backlog is DB2 and the evidence probe needs the corpus, neither of which
 * the daemon has. */
char *kb_client_learning_resolve_json(int budget);

/* Ask which policy arm to apply (S6). The bandit lives in the knowledge
 * service, so the daemon renders the fragment but does not choose it. NULL
 * or an unparseable answer means "use the local default". */
char *kb_client_learning_policy_select_json(const char *decision_point);

/* Fetch a single learning proposal via the aimee-kb sidecar.  Sends
 * `learning.get_proposal` with {id} and returns the heap-allocated JSON
 * response (caller frees).  {"status":"ok","proposal":{...}} on success. */
char *kb_client_learning_get_proposal_json(int id);

/* Accept a learning proposal via the aimee-kb sidecar.  Sends
 * `learning.accept_proposal` with {id} and returns the heap-allocated
 * JSON response (caller frees). */
char *kb_client_learning_accept_proposal_json(int id);

/* Reject a learning proposal via the aimee-kb sidecar.  Sends
 * `learning.reject_proposal` with {id} and returns the heap-allocated
 * JSON response (caller frees). */
char *kb_client_learning_reject_proposal_json(int id);

/* Intelligence readiness queries via the v1 KB API.  Each returns a
 * heap-allocated JSON string (caller frees) of shape {"status":"ok",...}
 * on success or {"status":"error","message":"..."}. */
char *kb_client_calibrate_readiness_json(void);
char *kb_client_demote_check_json(void);
char *kb_client_ranker_export_view_json(void);
char *kb_client_ranker_fit_json(void);
char *kb_client_bandit_export_json(void);

/* POST /v1/intelligence/bandit/replay-record — records the output of
 * tools/bandit_replay.py as a benchmark_trace artifact.
 * `result_json` must be a JSON object string (the replay-tool result). */
char *kb_client_bandit_replay_record_json(const char *decision_point, const char *result_json);

/* Server-side decision points reach the kb DB2 bandit through these. sample
 * selects+logs an arm (returns 0 + fills arm_out/decision_id_out, or -1 when
 * disabled/transport-failed); close records the reward [0,1] (best-effort). */
int kb_client_bandit_sample(const char *decision_point, const char *const *arms, int n_arms,
                            char *arm_out, size_t arm_out_len, char *decision_id_out,
                            size_t decision_id_out_len);
int kb_client_bandit_close(const char *decision_point, const char *decision_id, const char *arm_id,
                           double reward);
/* Persist the production-default arm for a decision point; raw kb JSON
 * ({status, rollback_arm}), caller frees. */
char *kb_client_bandit_promote_json(const char *decision_point, const char *arm);

/* artifacts.list_proposed: list proposed artifacts by surface (NULL = all).
 * Returns heap-allocated JSON string; caller frees. */
char *kb_client_artifacts_list_proposed_json(const char *target_surface, int limit);

/* artifacts.set_state: transition an artifact to committed or rejected.
 * verdict_tag/scope/counter_example/reason may be NULL.
 * Returns heap-allocated JSON string; caller frees. */
char *kb_client_artifact_set_state_json(const char *id, const char *new_state,
                                        const char *verdict_tag, const char *verdict_scope,
                                        const char *counter_example, const char *reason);

/* Gather memory + kb collection state for `aimee memory verify`.  Sends
 * `memory.verify` with {detail, timings, embedding_command} and returns the
 * heap-allocated JSON response (caller frees).  The response shape contains:
 *   server_version, active_embedder_version, embedder{dim},
 *   memory{collection, collection_exists, db2_memories, db2_units, vector_points,
 *          indexed_fields},
 *   kb{collection, collection_exists, db2_chunks, vector_points, indexed_fields},
 *   index_ops{ok, pending, failed, stuck},
 *   failed_ops[] (when detail=1), timings{trials,total_us,max_us} (when
 *   timings=1).  On any failure {"status":"error","message":"..."}. */

/* List the 100 most recent scenes (memory_scenes).  Returns
 * {"status":"ok","scenes":[{id, workspace_id, turn_count, created_at}, ...]}. */

/* List members of a single scene.  Returns
 * {"status":"ok","scene_id","members":[{memory_id,key,membership_strength}, ...]}. */

/* --- Canonical index thin-client RPCs --------------------------------
 *
 * Reads (find/list/blast_radius) and writes (scan) for the project /
 * org-level code index. aimee-kb is the only writer; these helpers are
 * the only sanctioned way for non-kb code to reach the canonical
 * index. If the knowledge service is unavailable, every helper returns -1 / 0 with
 * the output zeroed; the caller treats that as "no index available"
 * and proceeds without it (e.g. hooks skip background scans, queries
 * fall back to the branch overlay or report no results).
 *
 * No helper here will auto-spawn aimee-kb. */

typedef struct
{
   int skipped;   /* 1 if the scan was not run (busy / cooldown / kb down / kb error) */
   int projects;  /* number of projects scanned (0 when skipped) */
   int files;     /* number of files (re)indexed (0 when skipped) */
   int inspected; /* number of files visited (>= files); 0 if older kb */
   int retracted; /* files removed by a sealed complete manifest */
   long long index_revision;
   long retry_after; /* seconds until cooldown ends (0 when not in cooldown) */
   char reason[32];  /* "busy" | "cooldown" | "no_kb" | "error" | "" */
   char index_state[16];
   char workspace_state[16];
   char verification[16];
   char message[256]; /* human-readable detail; populated when reason == "error" */
} kb_client_index_scan_result_t;

/* Request a scan of one project, or all configured workspaces if both
 * `name` and `root` are NULL. Returns 0 on transport success (check
 * out->skipped); returns -1 when aimee-kb is unreachable (out->reason
 * = "no_kb") or when aimee-kb returned a service-level error (out->reason
 * = "error", out->message holds the kb-supplied detail). */
int kb_client_index_scan(const char *name, const char *root, int force,
                         kb_client_index_scan_result_t *out);

/* Stable project lifecycle. operation is detach|purge|gc. Empty confirm_hash
 * performs a read-only dry-run for purge/gc. Returns the raw JSON body (caller
 * frees) and writes the HTTP status when requested. */
char *kb_client_index_project_lifecycle_json(const char *operation, const char *project,
                                             const char *confirm_hash, const char *reason,
                                             int retention_days, int *http_status);

/* Push a set of caller-supplied source files to aimee-kb for indexing,
 * bypassing server-side filesystem enumeration. `files_arr_v` is a cJSON array
 * of {"rel_path","content"} objects (typed void * to keep this header
 * cJSON-free); it is ADOPTED (freed by this call) and may be NULL to let the kb
 * scan its own filesystem. Used by the thin-client workspace push path, where
 * the server cannot see the client's tree. Same return contract as
 * kb_client_index_scan. */
int kb_client_code_scan_push(const char *name, const char *root, int force, void *files_arr_v,
                             kb_client_index_scan_result_t *out);

/* Relay one phase of a complete client-owned scan. phase is begin|stage|seal|abort;
 * scan_id identifies the private session and expected_files is used only by
 * seal. A stage adopts files_arr_v using the same array shape as the legacy
 * push helper. This is the server-side half of remote thin-client ingestion. */
int kb_client_code_scan_phase(const char *name, const char *root, int force, const char *phase,
                              const char *scan_id, int expected_files, void *files_arr_v,
                              kb_client_index_scan_result_t *out);

/* Read-only canonical-index/workspace comparison. Returns response JSON owned
 * by the caller, or NULL on transport failure. */
char *kb_client_index_verify_json(const char *project, const char *root, int deep,
                                  int *http_status);

/* Internal: map a parsed aimee-kb response into the result struct.
 * Exposed so unit tests can pin the wire contract directly without
 * spinning up a fake socket. `resp` is a const cJSON * (typed as void *
 * here so callers needn't include cJSON.h). resp == NULL signals
 * transport failure. Returns 0 on a usable response, -1 on transport
 * failure or kb-side error. */
int kb_client_index_scan_apply_response(const void *resp, kb_client_index_scan_result_t *out);

/* Blast-radius contract checks, split from the transport so a recorded kb
 * payload can be asserted against without a live kb. `why` receives the first
 * failing term ("resolved", "dependent_edges", ...) so a rejection names
 * itself. Returns 1 when the payload satisfies the contract. */
int kb_client_index_blast_response_valid(const void *resp, char *why, size_t why_n);
int kb_client_index_blast_edges_valid(const void *edges, const char *identity_field);

/* Timeout the code-index scan POST uses, in ms. Default 5 minutes; raise with
 * AIMEE_KB_SCAN_TIMEOUT_MS for trees whose scan legitimately runs longer.
 * Values outside (0, 24h] are ignored so a typo cannot disable the bound. */
int kb_client_index_scan_timeout_ms(void);
int kb_client_index_read_timeout_ms(void);

/* Whether a failed call is the caller's budget expiring rather than the KB
 * being unreachable. A read timeout and a refused connection look identical
 * on the wire (no body, no status), so elapsed time against the budget is
 * what separates 'nobody answered' from 'someone is still working'. A
 * timeout must not open the shared dependency breaker. */
/* Failure-budget classes. Bulk work (ingest, embed) and interactive work (reads,
 * lookups) keep separate breakers so neither can suppress the other. */
typedef enum
{
   KB_DEP_INTERACTIVE = 0,
   KB_DEP_BULK = 1,
   KB_DEP_CLASS_COUNT = 2
} kb_dependency_class_t;

kb_dependency_class_t kb_dependency_class_for_path(const char *path);

int kb_transport_call_timed_out(int http_status, const char *response, int64_t elapsed_ms,
                                int timeout_ms);

/* Internal: build the wire-level response object that aimee-server's
 * handle_index_scan returns to the CLI, given the kb_client result and
 * its rc. Pulled out so dispatch and tests share one truth about
 * "ok with skipped+reason vs. error with message". Returns a heap
 * cJSON * (cast through void * to keep this header cJSON-free). */
void *kb_client_index_scan_format_response(int kb_rc, const kb_client_index_scan_result_t *res);

/* Find an identifier in the canonical index. Returns count of hits written
 * into `out` (capped at `max`), or -1 if the KB is unavailable. */
int kb_client_index_find(const char *identifier, term_hit_t *out, int max);
int kb_client_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max);
int kb_client_index_find_scoped(const char *preferred_project, int all_projects,
                                const char *identifier, term_hit_t *out, int max);

/* List indexed projects. Returns count on success (0 = empty index),
 * or -1 if the knowledge service is unreachable. */
int kb_client_index_list(project_info_t *out, int max);

/* Fast, non-spawning liveness probe for the knowledge service. Returns 1 when
 * a kb service is already reachable (live local socket, or a configured remote
 * endpoint), 0 otherwise. Unlike the index/search calls this never autostarts
 * aimee-kb, so latency-sensitive read paths (e.g. workspace list) can skip kb
 * enrichment and degrade gracefully instead of blocking on a 15s autostart when
 * the service is down. */
int kb_client_is_live(void);

/* Configuration presence, independent of reachability or the circuit breaker. */
int kb_client_connection_configured(void);
/* The standalone server supplies its private code owner. Registration happens
 * before request workers start; KB deployments retain their remote transport. */
typedef char *(*kb_client_local_code_fn)(const char *path, const void *body, int *status);
void kb_client_set_local_code_provider(kb_client_local_code_fn provider);
int kb_client_local_code_enabled(void);

/* Compute blast radius for a file. Returns 0 on success (out is
 * filled), -1 if kb is unreachable or the canonical lookup failed. */
int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out);

/* Compute aggregated blast-radius preview for up to 100 files through
 * /v1 blast-radius reads. Returns a heap-allocated JSON string (caller frees) of shape
 * {"status":"ok","total_dependents":N,"severity":"...","files":[...],
 *  "warnings":[...]}, or NULL if kb is unreachable. */
char *kb_client_index_blast_radius_preview_json(const char *project, char **paths, int path_count);

/* List the structural definitions in a single file (function/struct/etc).
 * Returns count written into `out` (capped at `max`), or -1 if the KB is
 * unavailable. Mirrors index_structure(). */
int kb_client_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max);

/* Find call sites for a symbol.  `project` may be NULL/empty to search
 * across all indexed projects.  Returns count, or 0 if kb is unreachable.
 * Uses /v1 over remote HTTP or local UDS. Mirrors index_find_callers(). */
int kb_client_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max);
int kb_client_index_find_callers_scoped(const char *preferred_project, int all_projects,
                                        const char *symbol, caller_hit_t *out, int max);

/* S6: cross-repo dependency edges for `project` (required). `direction`
 * (out|in|both), `min_tier` (high|medium|tentative) may be NULL/empty for the kb
 * defaults; status_ambiguous!=0 requests the AMBIGUOUS review queue instead of
 * edges; dry_run!=0 requests offline candidate inspection (all bands + inline
 * ambiguous, no writes). Returns the raw kb JSON body (caller frees) or NULL if kb
 * unreachable. */
char *kb_client_index_cross_repo_deps_json(const char *project, const char *direction,
                                           const char *min_tier, int status_ambiguous, int dry_run);

/* Full-text code search across indexed file contents.  `project` may be
 * NULL/empty to search all projects.  Returns count, or 0 if kb is
 * unreachable.  Uses /v1 over remote HTTP or local UDS.
 * Mirrors index_code_search(). */
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max);
int kb_client_index_code_search_scoped(const char *query, const char *preferred_project,
                                       int all_projects, code_search_hit_t *out, int max);

/* Structured-PDF evidence routes (/v1/pdf/...). Each returns the route's verbatim
 * citation JSON as a malloc'd string the caller frees, or NULL on a
 * parameter/transport/non-2xx failure; *status_out (may be NULL) receives the
 * HTTP status. The access-control invariant (doc_kind='pdf',
 * quarantine_state<>'pending', project scoping) is enforced route/DB-side.
 * ALL four require a non-empty project: it scopes the DB query and is what makes
 * the kb_http token-scope gate fire, so an un-scoped call cannot read across
 * projects ("project-scope all PDF reads"). */
char *kb_client_pdf_search_chunks(const char *query, const char *project, int max_results,
                                  int *status_out);
char *kb_client_pdf_open_page(const char *project, const char *document_key, int page_no,
                              int *status_out);
char *kb_client_pdf_open_neighbors(const char *project, long long chunk_id, int *status_out);
char *kb_client_pdf_inspect_structure(const char *project, const char *document_key,
                                      int *status_out);
/* Phase B: GET /v1/pdf/lookup_table — structured table cells for a document (page_no < 0 =
 * all pages). Returns the route JSON body verbatim. */
char *kb_client_pdf_lookup_table(const char *project, const char *document_key, int page_no,
                                 int *status_out);
/* Phase C: GET /v1/pdf/assets (list a doc's crop assets) + GET /v1/pdf/open_asset (stream one
 * crop's bytes, base64, for an opaque asset id). Route JSON body verbatim. */
char *kb_client_pdf_list_assets(const char *project, const char *document_key, int *status_out);
char *kb_client_pdf_open_asset(const char *project, long long asset_id, int *status_out);

/* Code-graph retrieval + analytics routes (see kb_client_code_graph.c). Forward
 * the route's JSON body verbatim (nested fused/ranked shapes the agent consumes
 * directly); return the malloc'd JSON (caller frees) or NULL, with *status_out
 * carrying the HTTP status. /v1/code/hybrid fuses code+graph+memory (query
 * required, symbol/project optional); /v1/code/graph/hubs ranks a project's
 * most-connected symbols (project required). */
char *kb_client_code_hybrid(const char *query, const char *symbol, const char *project,
                            int max_results, int *status_out);
char *kb_client_code_hybrid_scoped(const char *query, const char *symbol,
                                   const char *preferred_project, int all_projects, int max_results,
                                   int *status_out);
/* Strict active-project task packet. The route fixes max_results=4 and its
 * resident budget at 1200 tokens; project is required and never broadens. */
char *kb_client_code_context(const char *query, const char *symbol, const char *project,
                             int *status_out);
char *kb_client_code_graph_hubs(const char *project, int max_results, int *status_out);
char *kb_client_code_graph_audit(const char *project, int max_findings, int *status_out);
char *kb_client_code_lessons(const char *project, int *status_out);
int kb_client_code_lessons_observe(const char *project, const char *session_id,
                                   const char *const *node_ids, int n_nodes);
char *kb_client_code_graph_diff(const char *project, const char *from_gen, const char *to_gen,
                                int force, int *status_out);
/* /v1/code/graph/surprising: file pairs that are semantically close yet
 * structurally far (project required). `judge` opts into the LLM confirmation. */
char *kb_client_code_graph_surprising(const char *project, int max_results, int judge,
                                      int *status_out);
/* /v1/code/graph: a node's incident projection edges (callers/callees/neighbors);
 * project + node required. Backs the webchat graph view (§8). */
char *kb_client_code_graph_node(const char *project, const char *node, int max_results,
                                int *status_out);

/* Counts of indexed files / definition-kind terms for one project.
 * Uses /v1 over remote HTTP or local UDS.
 * Returns 0 on success and writes to the out pointers (either may be NULL),
 * -1 if kb is unreachable.  Mirrors canonical_index_project_stats(). */
int kb_client_index_project_stats(const char *project, int *files_out, int *defs_out);

/* Language breakdown for one project: writes a JSON array of {lang,count}
 * objects sorted by count desc (up to 8 entries) into buf. Returns 0 on
 * success, -1 if kb is unreachable. Uses /v1 over remote HTTP or local UDS.
 * Mirrors canonical_index_project_lang_breakdown(). */
int kb_client_index_project_lang(const char *project, char *buf, size_t bufsz);

int kb_client_canonical_index_scan(const char *project, const char *root_path, int force);

/* Request-local active repository context for ordered memory RPCs.  The
 * server sets this around one agent-facing call; legacy callers that do not
 * set it retain their existing unscoped wire semantics. */
void kb_client_memory_scope_context_set(const char *workspace, const char *project,
                                        int include_all);
void kb_client_memory_scope_context_clear(void);
void kb_client_memory_scope_context_apply(cJSON *req);
/* Consumes req; optional host file is bounded and sent as unchanged text. */
char *kb_client_memory_benchmark_json(cJSON *req, const char *corpus_path);

#endif /* DEC_KB_CLIENT_H */

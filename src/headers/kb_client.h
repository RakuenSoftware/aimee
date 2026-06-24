#ifndef DEC_KB_CLIENT_H
#define DEC_KB_CLIENT_H 1

#include "anti_patterns.h" /* anti_pattern_t */
#include "decision_log.h"  /* db2_decision_log_row_t */
#include "index.h"         /* project_info_t, term_hit_t, blast_radius_t */
#include "memory.h"        /* memory_t, edge_t */
#include "entity_edges.h"  /* db2_relation_schema_row_t */
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
   char last_maintenance_at[64];
   int last_maintenance_rows_decayed;
   int last_maintenance_orphans_pruned;
   int maintenance_enabled;
} kb_health_t;

/* Query aimee-kb health.  Fills *out and returns 0 on success.  Returns -1
 * if aimee-kb is unreachable (out->process_ok == 0). */
int kb_client_health(kb_health_t *out);

/* Returns the raw /v1/health JSON document (caller frees), or NULL if aimee-kb
 * is unreachable or returns non-2xx. Used by `aimee kb curator status` to read
 * the curator block (richer than the flat kb_health_t snapshot). */
char *kb_client_health_json(void);
/* §2c: POST /v1/reembed; raw response JSON (caller frees) or NULL on transport
 * failure; *status_out (optional) gets the HTTP status. target_dim>0 pins the
 * reset target (bypasses the embedder probe); clear_maintenance!=0 instead just
 * force-clears a stuck reembed_in_progress marker (ignores confirm/force/dim). */
char *kb_client_reembed(int confirm, int force, int dry_run, int target_dim, int clear_maintenance,
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

/* Run a KB rebuild (force repair) via the public /v1/maintenance/repair
 * endpoint with the given path/project/embedding_command and returns the
 * heap-allocated JSON response (caller frees).  On any failure the returned
 * JSON has {"status":"error","message":"..."}.  Repair is long-running on
 * large repos; the request timeout reflects that. */
char *kb_client_repair_json(const char *path, const char *project, const char *embedding_command);

/* Clear all KB chunks for a project via the public /v1/maintenance/clear
 * endpoint and returns the heap-allocated JSON response (caller frees).
 * On any failure the returned JSON has {"status":"error","message":"..."}. */
char *kb_client_clear_json(const char *project);

/* Run a KB search via aimee-kb's public /v1/search endpoint with the given
 * project / query / embedding_command / max_results / format ("text" or
 * "json") and returns the heap-allocated JSON response envelope (caller
 * frees).  The result payload lives at
 * resp["result"] and is either the plain-text or JSON search output
 * depending on `format`.  On any failure the returned JSON has
 * {"status":"error","message":"..."}. */
/* Search KB documents via aimee-kb.  |project| may be NULL or empty
 * to search across all projects (the agent search_docs tool calls
 * it that way).  |format| selects "text" (default) or "json"
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

/* Rebuild derived retrieval indexes for memories.  Sends
 * `memory.reindex` with {limit} and returns the heap-allocated JSON
 * response (caller frees).  A limit of 0 means "as many as the
 * server wants to do in one call". */
char *kb_client_memory_reindex_json(int limit);

/* Rebuild the memory vector index from DB2 embeddings for a
 * specific embedder version.  Sends `memory.rebuild` with {version}
 * (version may be empty/NULL, in which case aimee-kb resolves the
 * active embedder) and returns the heap-allocated JSON response
 * (caller frees). */
char *kb_client_memory_rebuild_json(const char *version);

/* Create a memory directive via the aimee-kb sidecar.  Sends
 * `memory.directive_create` with the usual field set and returns
 * {"status":"ok","dedup":0|1,"directive":{...}} on success.  dedup=1
 * means the server rejected as duplicate of an existing row (no
 * directive object returned).  Caller frees. */
char *kb_client_memory_directive_create_json(const char *question, const char *topic,
                                             const char *entity, const char *file,
                                             const char *cause, int priority, const char *session,
                                             const char *valid_until);

/* Transition an open directive to resolved.  Sends
 * `memory.directive_resolve` with {id, with_memory, note}.  Caller frees. */
char *kb_client_memory_directive_resolve_json(int64_t id, int64_t with_memory, const char *note);

/* Mark an open directive as suppressed.  Sends
 * `memory.directive_suppress` with {id}.  Caller frees. */
char *kb_client_memory_directive_suppress_json(int64_t id);

/* Sweep expired directives.  Returns {"status":"ok","expired":N}.  Caller
 * frees. */
char *kb_client_memory_directive_sweep_expired_json(void);

/* List directives filtered by state/cause with a cap of `limit` rows.
 * Returns {"status":"ok","directives":[...]} on success.  Caller frees. */
char *kb_client_memory_directive_list_json(const char *state, const char *cause, int limit);

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
 * directly. Defined in src/server/kb_client_roadmap.c. */
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

/* Session-prune helpers wrapped on the kb side so prune_stale_sessions
 * (CLI-fork) actually runs the maintenance work.  Each returns the
 * count of rows acted on, or -1 if kb is unreachable.  Mirrors
 * memory_fold_session(). */
int kb_client_memory_fold_session(const char *session_id);

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

/* Search stored memory facts via aimee-kb (the DB2 owner).  Returns
 * the number of rows written into |out| (0..|max|), or -1 if kb is
 * unreachable or the vector index is unavailable.  Mirrors
 * memory_find_facts() in shape so daemon-side handlers can swap the
 * direct call for this RPC without changing their downstream code. */
int kb_client_memory_find_facts(const char *query, int limit, memory_t *out, int max);

/* Like kb_client_memory_find_facts but with an explicit graph_code_fusion_state
 * field. graph_code_fusion_state: "off" | "shadow" | "on"; NULL defaults to
 * "off". Recall is always fusion-on in production (the plain
 * kb_client_memory_find_facts forwards "on"); this _ex form exists for the
 * eval/benchmark harness, which forwards "off"/"shadow" to measure on-vs-off. */
int kb_client_memory_find_facts_ex(const char *query, int limit, memory_t *out, int max,
                                   const char *graph_code_fusion_state);

/* List stored memories filtered by tier/kind via aimee-kb.  Returns
 * the number of rows written into |out| (0 if kb is unreachable).
 * Mirrors memory_list(). */
int kb_client_memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max);

/* Load the eval-corpus memories via aimee-kb (DB2 owner).  Returns the
 * number of rows written into |out| (0 on failure / empty corpus).
 * Mirrors db2_memory_load_eval_corpus(). */
int kb_client_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len);

/* Top L2 facts via aimee-kb.  Returns row count.  Mirrors
 * db2_memory_top_l2_facts(). */
int kb_client_memory_top_l2_facts(memory_t *out, int max);

/* Render the "Open Commitments" / "Unresolved Questions" briefing
 * sections via aimee-kb.  Returns a heap-allocated markdown fragment
 * (caller frees) or NULL when the section is empty / kb is unreachable.
 * Mirror session_briefing_render_commitments / _directives. */
char *kb_client_session_briefing_commitments(int limit);
char *kb_client_session_briefing_directives(int limit);

/* Prospective memory CRUD via aimee-kb.  Each returns the kb response
 * envelope as JSON (caller frees) or NULL when kb is unreachable.
 * Mirror memory_prospective_list / _create / _complete. */
char *kb_client_memory_prospective_list_json(const char *state, int limit);
char *kb_client_memory_prospective_create_json(const char *trigger_text, const char *action_text,
                                               const char *anchor_entity, const char *anchor_file,
                                               const char *recurrence, const char *valid_until);
char *kb_client_memory_prospective_complete_json(int64_t id);

/* Match active prospective memories against the current turn / entity / file
 * via aimee-kb. Returns count written into `out` (capped at `max`), or 0 if
 * kb is unreachable. Mirrors memory_prospective_match(). */
int kb_client_memory_prospective_match(const char *turn_text, const char *active_entity,
                                       const char *active_file, memory_prospective_t *out, int max);

/* Bump the trigger counter on a prospective memory via aimee-kb.  Returns
 * 0 on success, -1 on failure or if kb is unreachable.  Mirrors
 * memory_prospective_mark_triggered(). */
int kb_client_memory_prospective_mark_triggered(int64_t id);

/* Sweep expired prospective memories via aimee-kb.  Returns the number
 * expired (0 on failure / kb unreachable).  Mirrors
 * memory_prospective_sweep_expired(). */
int kb_client_memory_prospective_sweep_expired(void);

/* Run memory maintenance (replay/compact/prune/summarize) inside aimee-kb.
 * Returns the kb response envelope as JSON (caller frees) including the
 * summary object.  Mirrors memory_maintenance_run(). */
char *kb_client_memory_maintenance_run_json(unsigned int modes, int force, int dry_run);
char *kb_client_memory_lint_json(void);

/* Memory alerts / session recall via aimee-kb.  Each returns the kb
 * response envelope as JSON (caller frees) with the bundle nested under
 * "alerts" or "recall".  Mirror memory_alerts / memory_recall. */
char *kb_client_memory_alerts_json(const char *since);
char *kb_client_memory_recall_json(const char *task_hint, int limit_tokens, int session_start);
/* As above, with an explicit graph-code fusion state ("off"/"shadow"/"on", NULL
 * = off). Production recall always forwards "on" (via kb_client_memory_recall_json);
 * this _ex form lets the eval/benchmark harness force a different state. */
char *kb_client_memory_recall_json_ex(const char *task_hint, int limit_tokens, int session_start,
                                      const char *graph_code_fusion_state);

/* Upsert a workflow:<workspace>:<signal_type> memory via aimee-kb.
 * Returns the new memory id (>0) or -1 on failure / kb unreachable.
 * Mirrors memory_upsert_workflow(). */
int64_t kb_client_memory_upsert_workflow(const char *workspace, const char *signal_type,
                                         const char *rule, double observed_confidence,
                                         const char *session_id);

/* Read provenance entries for a memory via aimee-kb.  Returns count
 * written into `out` (capped at `max`), or 0 if kb is unreachable.
 * Mirrors memory_get_provenance(). */
int kb_client_memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max);

/* Apply a workspace / scope tag to a memory via aimee-kb.  Both return
 * 0 on success, -1 on failure or if kb is unreachable.  Mirror
 * memory_tag_workspace / memory_tag_scope. */
int kb_client_memory_tag_workspace(int64_t memory_id, const char *workspace);
int kb_client_memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value);

/* Generate (and store) an episode card summarising one session via aimee-kb.
 * Returns the new memory_unit row id on success, 0 on failure or when
 * episode summarisation is disabled in config.  Mirrors
 * memory_episode_card_generate(). */
int64_t kb_client_memory_episode_card_generate(const char *source_session);

/* Compute the scope-visibility rank (0-3) for each memory id in `ids`
 * (length `id_count`), filling `out_ranks` with the per-id rank.  Returns
 * the number of ranks filled (0 if kb is unreachable).  Mirrors per-id
 * memory_scope_visibility_rank() in batch form. */
int kb_client_memory_scope_visibility_rank(const int64_t *ids, int id_count, const char *workspace,
                                           const char *project, int *out_ranks);

/* Memory-to-memory link CRUD via aimee-kb.  Mirror memory_link_create
 * / _query / _delete.  Return 0/-1 on success/failure for create+delete,
 * row count for query (0 on failure / kb unreachable). */
int kb_client_memory_link_create(int64_t source_id, int64_t target_id, const char *relation);
int kb_client_memory_link_query(int64_t memory_id, memory_link_t *out, int max);
int kb_client_memory_link_delete(int64_t link_id);

/* Delete a memory by id via aimee-kb.  Returns 0 on success, -1 on
 * failure / kb unreachable.  Mirrors memory_delete(). */
int kb_client_memory_delete(int64_t id);

/* Increment use_count and stamp last_used_at (positive reinforcement).
 * Returns 0 on success, -1 on failure / kb unreachable. */
int kb_client_memory_touch(int64_t id);

/* Replace a memory's content in place (update verb).
 * Returns 0 on success, -1 on failure / kb unreachable. */
int kb_client_memory_update(int64_t id, const char *content);

/* Apply negative reinforcement: reduce confidence by 0.1 (floor 0.0).
 * Optional reason is recorded for audit.
 * Returns 0 on success, -1 on failure / kb unreachable. */
int kb_client_memory_reject(int64_t id, const char *reason);

/* Read aggregate memory stats via aimee-kb.  Returns 0 on success,
 * -1 on failure / kb unreachable.  Mirrors memory_stats(). */
int kb_client_memory_stats(memory_stats_t *out);

/* Raw-JSON variant: returns the kb "stats" object as a malloc'd JSON string
 * (caller frees), or NULL when the kb is unreachable / returns a non-"ok"
 * envelope.  Used by handle_memory_stats to forward the payload verbatim. */
char *kb_client_memory_stats_json(void);

/* List unresolved conflicts via aimee-kb.  Returns row count.
 * Mirrors memory_list_conflicts(). */
int kb_client_memory_list_conflicts(conflict_t *out, int max);

/* Read 7-day rolling health stats via aimee-kb.  Returns 0 / -1.
 * Mirrors memory_query_health(). */
int kb_client_memory_query_health(memory_health_t *out);

/* Read effectiveness stats via aimee-kb.  Returns 0 / -1.
 * Mirrors memory_effectiveness_stats(). */
int kb_client_memory_effectiveness_stats(effectiveness_stats_t *out);

/* List entity-graph edges incident to |entity| via aimee-kb.  Returns
 * row count.  Mirrors memory_query_edges(). */
int kb_client_memory_query_edges(const char *entity, edge_t *out, int max);

/* Compact conversation windows (raw->summary, summary->fact) via aimee-kb.
 * Returns 0 / -1.  Mirrors memory_compact_windows(). */
int kb_client_memory_compact_windows(int *summary_count, int *fact_count);

/* Assemble the agent context block via aimee-kb.  Returns a heap-allocated
 * markdown string (caller frees) or NULL on failure / kb unreachable.
 * Mirrors memory_assemble_context(). */
char *kb_client_memory_assemble_context(const char *task_hint);

/* Search conversation windows via aimee-kb.  Returns row count.
 * Mirrors memory_search(). */
int kb_client_memory_search(char **clusters, int cluster_count, int limit, search_result_t *out,
                            int max);

/* Workspace/project-scoped fact search via aimee-kb.  Returns row count.
 * Mirrors memory_find_facts_visible() / memory_find_facts_scoped(). */
int kb_client_memory_find_facts_visible(const char *query, const char *workspace,
                                        const char *project, int limit, memory_t *out, int max);
int kb_client_memory_find_facts_scoped(const char *query, const char *scope_type,
                                       const char *scope_value, int limit, memory_t *out, int max);
/* As above, with an explicit graph-code fusion state ("off"/"shadow"/"on", NULL
 * = off). Production scoped recall always forwards "on" (via
 * kb_client_memory_find_facts_scoped); this _ex form is for the eval harness. */
int kb_client_memory_find_facts_scoped_ex(const char *query, const char *scope_type,
                                          const char *scope_value, int limit, memory_t *out,
                                          int max, const char *graph_code_fusion_state);

/* Export memories / decisions to a JSONL file via aimee-kb.  Returns
 * row count or -1.  Mirrors the wholesale-export flow used by
 * `aimee export`. */
int kb_client_memory_export_jsonl(const char *path);
int kb_client_memory_decisions_export_jsonl(const char *path);

/* Check whether a memory key already exists via aimee-kb.  Returns 1
 * if present, 0 otherwise (or on kb-unreachable).  Mirrors
 * db2_memory_key_exists(). */
int kb_client_memory_key_exists(const char *key);

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

/* List the memory_relation_schema rows owned by aimee-kb.  Writes up
 * to |max| rows into |out| and returns the number written. */
int kb_client_relations_schema_list(db2_relation_schema_row_t *out, int max);

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

/* memory_cognify_drain crosses DB1 queue state and DB2 memory reads. Ports
 * must split that flow across aimee-server and aimee-kb; do not add a client
 * or auxiliary process with both tiers linked. */

/* Fetch a single memory row by id via aimee-kb.  Returns 0 on
 * success (out is filled) or -1 if kb is unreachable or the row is
 * missing.  Mirrors memory_get(). */
int kb_client_memory_get(int64_t id, memory_t *out);

/* Insert a memory row via aimee-kb (the DB2 owner).  The full
 * write-side gate pipeline runs inside aimee-kb.  Returns 0 on
 * success (|out| filled if non-NULL) or -1 if kb is unreachable or
 * the gate rejected the write.  Mirrors memory_insert(). */
int kb_client_memory_insert(const char *tier, const char *kind, const char *key,
                            const char *content, double confidence, const char *session_id,
                            memory_t *out);
int kb_client_memory_insert_ex(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_t *out);

/* Look up a memory id by (key, kind) via aimee-kb.  Returns 0 if no
 * row matches or kb is unreachable; the row id otherwise.  Mirrors
 * db2_memory_find_id_by_key_kind(). */
int64_t kb_client_memory_find_id_by_key_kind(const char *key, const char *kind);

/* Supersede an existing memory with a new content via aimee-kb (the
 * DB2 owner).  The kb side runs the full version-bump + provenance +
 * link pipeline.  Returns 0 on success (out filled if non-NULL) or
 * -1 if kb is unreachable / supersede failed.  Mirrors
 * memory_supersede(). */
int kb_client_memory_supersede(int64_t old_id, const char *new_content, double confidence,
                               const char *session_id, memory_t *out);

/* Fetch the version history for a memory key via aimee-kb.  Returns
 * the number of rows written into |out| (0 if kb is unreachable).
 * Mirrors memory_fact_history(). */
int kb_client_memory_fact_history(const char *key, memory_t *out, int max);

/* Stale-memory inspection helpers via aimee-kb (the DB2 owner).
 * Each mirrors the local db2_memory_list_* signature; returns the
 * number of rows written (0 if kb is unreachable). */
int kb_client_memory_list_low_effectiveness(double threshold, int limit,
                                            db2_memory_low_eff_row_t *out, int max);
int kb_client_memory_list_unused_l2(int days, db2_memory_unused_l2_row_t *out, int max);
int kb_client_memory_list_superseded_keys(int min_versions, db2_memory_superseded_row_t *out,
                                          int max);

/* Set the artifact_type / artifact_ref / artifact_hash columns on a
 * memory row via aimee-kb.  Returns 0 on success, -1 on failure /
 * missing row.  Mirrors db2_memory_set_artifact(). */
int kb_client_memory_set_artifact(int64_t memory_id, const char *artifact_type,
                                  const char *artifact_ref, const char *artifact_hash);

/* Session-scope priority memory listings via aimee-kb.  Used by the
 * session-start prompt builder to populate # Project / # Workspace /
 * # Global Context sections.  Each returns the number of rows
 * written into |out| (0 if kb is unreachable).  Mirrors
 * db2_memory_list_session_scope_priority{,_like}(). */
int kb_client_memory_list_session_scope_priority(memory_t *out, int max);
int kb_client_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max);

/* Run the active-task drift check via aimee-kb.  Returns 0 on
 * success (|out| filled) or -1 if kb is unreachable / task missing.
 * Mirrors memory_check_drift(); see headers/memory.h for
 * drift_result_t. */
int kb_client_memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                                 drift_result_t *out);

/* Search facts/patterns by free-text keyword via aimee-kb.  Returns
 * the number of rows written into |out| (0 if kb is unreachable).
 * Mirrors db2_memory_search_facts_patterns_by_keyword(). */
int kb_client_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max);

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

/* Build the memory briefing bundle via aimee-kb.  Returns a heap-
 * allocated cJSON object (caller cJSON_Delete()s) or NULL on failure.
 * Mirrors memory_briefing(). */
struct cJSON *kb_client_memory_briefing(int limit_tokens);

/* Fetch a context block via aimee-kb.  Returns a heap-allocated
 * string (caller frees) or NULL if kb is unreachable.  Mirrors
 * memory_get_context_block(). */
char *kb_client_memory_context_block(const char *query, const char *block_type, int limit);

/* Read-only typed-fact recall for the turn: facts about entities named in the
 * query, PII-gated. Returns the facts block (caller frees), NULL if kb is
 * unreachable or there are no facts. Cheaper than context_block (no memory
 * assembly); used by ingress_preinject to auto-inject known facts. */
char *kb_client_memory_facts(const char *query);

/* Auditable-correctness P1: ask the KB to record a single per-turn
 * retrieval_event keyed by `turn_id` (a UUID), listing the int64 memory row ids
 * surfaced into the turn. `role` is the recall op (e.g. "Recall"),
 * `query_fingerprint` identifies the turn query. Returns 0 on success (event
 * written), -1 on bad args or kb error. The KB write uses the already-merged
 * db2_demotion_retrieval_event_write_turn (first-wins on duplicate turn_id). */
int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids);

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

/* Fetch the entity profile card via aimee-kb.  Returns 0 on success
 * (|out| filled) or -1 if kb is unreachable or the entity is missing.
 * Mirrors memory_get_entity_profile(). */
int kb_client_memory_get_entity_profile(const char *entity, memory_entity_profile_t *out);

/* Fetch up to |max| graph edges for an entity via aimee-kb.  Returns
 * the number of edges written into |out| (0 if kb is unreachable).
 * Mirrors memory_get_entity_edges(). */
int kb_client_memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out,
                                      int max);

/* Search graph relations by free-text query via aimee-kb.  Returns
 * the number of relations written into |out| (0 if kb is unreachable).
 * Mirrors memory_search_graph(). */
int kb_client_memory_search_graph(const char *query, int limit, memory_relation_t *out, int max);

/* Search the entity graph as of a wall-clock timestamp via aimee-kb.
 * Returns row count.  Mirrors memory_search_graph_as_of(). */
int kb_client_memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                                        memory_relation_t *out, int max);

/* Fetch a single episode by key via aimee-kb.  Returns 0 on success
 * (out is filled) or -1 if kb is unreachable or the episode is
 * missing.  Mirrors memory_get_episode(). */
int kb_client_memory_get_episode(const char *episode_key, memory_episode_t *out);

/* Run the memory Q&A pipeline via aimee-kb (the DB2 owner).  Returns
 * 0 on success (|out| filled) or -1 if kb is unreachable or
 * memory_ask_query failed (out->error has the message).  Mirrors
 * memory_ask_query(). */
int kb_client_memory_ask(const char *query, const char *scope_type, const char *scope_value,
                         int limit, memory_answer_result_t *out);

/* Fetch learning proposals via the aimee-kb sidecar.  Sends
 * `learning.list_proposals` with {state, sink, limit} and returns the
 * heap-allocated JSON response (caller frees).  The response has
 * {"status":"ok","proposals":[...]} where each element is the
 * `learning_proposal_to_json` shape.  On any failure the response has
 * {"status":"error","message":"..."}. */
char *kb_client_learning_list_proposals_json(const char *state, const char *sink, int limit);

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

/* Replay vector upserts for memory points.  Sends `memory.repair` with
 * {limit, failed_only, reset_stuck, memory_id, embedding_command} and
 * returns the heap-allocated JSON response (caller frees).  Behaviour:
 *   reset_stuck=1   -> zero attempts on stuck vector-index rows.
 *   memory_id>0     -> repair exactly that memory.
 *   failed_only=1   -> repair rows surfaced by vector-index scan.
 *   otherwise       -> sweep the memories table (limit caps the sweep).
 * On any failure the returned JSON has {"status":"error","message":"..."}. */
char *kb_client_memory_repair_json(int limit, int failed_only, int reset_stuck, int64_t memory_id,
                                   const char *embedding_command);

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
char *kb_client_memory_verify_json(int detail, int timings, const char *embedding_command);

/* Embed one memory (memory_id>0) or all stale L1/L2 memories (all=1).  For
 * batch mode the caller must pass `version` — the active embedder version —
 * so aimee-kb does not need to consult CLI-side config.  Returns the
 * heap-allocated JSON response (caller frees). */
char *kb_client_memory_embed_json(int all, int64_t memory_id, const char *version,
                                  const char *embedding_command);

/* Begin or resume a versioned re-embed job.  The caller passes the target
 * version and embedder command; aimee-kb upserts memory_reembed_progress,
 * loops memory_embed over every stale memory, and returns counts.  This is
 * long-running for large corpora — plan timeouts accordingly. */
char *kb_client_memory_reembed_start_json(const char *version, const char *embedding_command);

/* Report active embedder version + current memory_reembed_progress row. */
char *kb_client_memory_reembed_status_json(void);

/* Activate the completed target version of the in-flight re-embed job.
 * Writes memory_active_embedder, marks memory_reembed_progress finished,
 * and rebuilds the vector index at the new version. */
char *kb_client_memory_reembed_cutover_json(void);

/* Activate an arbitrary previously-embedded version and rebuild the vector index at it. Fails
 * cleanly if the version has no pgvector memory rows. */
char *kb_client_memory_reembed_rollback_json(const char *version);

/* List the 100 most recent scenes (memory_scenes).  Returns
 * {"status":"ok","scenes":[{id, workspace_id, turn_count, created_at}, ...]}. */
char *kb_client_memory_scene_list_json(void);

/* List members of a single scene.  Returns
 * {"status":"ok","scene_id","members":[{memory_id,key,membership_strength}, ...]}. */
char *kb_client_memory_scene_show_json(int64_t scene_id);

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
   int skipped;       /* 1 if the scan was not run (busy / cooldown / kb down / kb error) */
   int projects;      /* number of projects scanned (0 when skipped) */
   int files;         /* number of files (re)indexed (0 when skipped) */
   int inspected;     /* number of files visited (>= files); 0 if older kb */
   long retry_after;  /* seconds until cooldown ends (0 when not in cooldown) */
   char reason[32];   /* "busy" | "cooldown" | "no_kb" | "error" | "" */
   char message[256]; /* human-readable detail; populated when reason == "error" */
} kb_client_index_scan_result_t;

/* Request a scan of one project, or all configured workspaces if both
 * `name` and `root` are NULL. Returns 0 on transport success (check
 * out->skipped); returns -1 when aimee-kb is unreachable (out->reason
 * = "no_kb") or when aimee-kb returned a service-level error (out->reason
 * = "error", out->message holds the kb-supplied detail). */
int kb_client_index_scan(const char *name, const char *root, int force,
                         kb_client_index_scan_result_t *out);

/* Push a set of caller-supplied source files to aimee-kb for indexing,
 * bypassing server-side filesystem enumeration. `files_arr_v` is a cJSON array
 * of {"rel_path","content"} objects (typed void * to keep this header
 * cJSON-free); it is ADOPTED (freed by this call) and may be NULL to let the kb
 * scan its own filesystem. Used by the thin-client workspace push path, where
 * the server cannot see the client's tree. Same return contract as
 * kb_client_index_scan. */
int kb_client_code_scan_push(const char *name, const char *root, int force, void *files_arr_v,
                             kb_client_index_scan_result_t *out);

/* Internal: map a parsed aimee-kb response into the result struct.
 * Exposed so unit tests can pin the wire contract directly without
 * spinning up a fake socket. `resp` is a const cJSON * (typed as void *
 * here so callers needn't include cJSON.h). resp == NULL signals
 * transport failure. Returns 0 on a usable response, -1 on transport
 * failure or kb-side error. */
int kb_client_index_scan_apply_response(const void *resp, kb_client_index_scan_result_t *out);

/* Internal: build the wire-level response object that aimee-server's
 * handle_index_scan returns to the CLI, given the kb_client result and
 * its rc. Pulled out so dispatch and tests share one truth about
 * "ok with skipped+reason vs. error with message". Returns a heap
 * cJSON * (cast through void * to keep this header cJSON-free). */
void *kb_client_index_scan_format_response(int kb_rc, const kb_client_index_scan_result_t *res);

/* Find an identifier in the canonical index. Returns count of hits
 * written into `out` (capped at `max`), or 0 if kb is unreachable. */
int kb_client_index_find(const char *identifier, term_hit_t *out, int max);

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

/* Compute blast radius for a file. Returns 0 on success (out is
 * filled), -1 if kb is unreachable or the canonical lookup failed. */
int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out);

/* Compute aggregated blast-radius preview for up to 100 files through
 * /v1 blast-radius reads. Returns a heap-allocated JSON string (caller frees) of shape
 * {"status":"ok","total_dependents":N,"severity":"...","files":[...],
 *  "warnings":[...]}, or NULL if kb is unreachable. */
char *kb_client_index_blast_radius_preview_json(const char *project, char **paths, int path_count);

/* List the structural definitions in a single file (function/struct/etc).
 * Returns count written into `out` (capped at `max`), or 0 if kb is
 * unreachable.  Mirrors index_structure(). */
int kb_client_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max);

/* Find call sites for a symbol.  `project` may be NULL/empty to search
 * across all indexed projects.  Returns count, or 0 if kb is unreachable.
 * Uses /v1 over remote HTTP or local UDS. Mirrors index_find_callers(). */
int kb_client_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max);

/* Full-text code search across indexed file contents.  `project` may be
 * NULL/empty to search all projects.  Returns count, or 0 if kb is
 * unreachable.  Uses /v1 over remote HTTP or local UDS.
 * Mirrors index_code_search(). */
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max);

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

#endif /* DEC_KB_CLIENT_H */

#ifndef DEC_AGENT_EXEC_H
#define DEC_AGENT_EXEC_H 1

#include "agent_types.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/tools/module_api.h>

struct cJSON; /* forward declaration for gemini_prompt_cache_attach */

/* Host startup installs the additional source-owned unknown-effect gate.
 * This callback cannot override a denial by the ordinary policy owners. */
typedef int (*policy_unknown_effect_gate_fn)(const char *, const char *, const char *, uint32_t,
                                             int, char *, size_t);
void policy_set_unknown_effect_gate(policy_unknown_effect_gate_fn gate);
void policy_unknown_effect_clear(void);
/* Execution */
void agent_reject_degenerate_plain_response(agent_result_t *out, const char *agent_name);
int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const aimee_request_t *request,
                                      agent_result_t *out);

int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const aimee_request_t *request, struct cJSON *initial_messages,
                                     struct cJSON **updated_messages, agent_result_t *out);

/* Complete canonical conversation for the server-owned tool loop. No user
 * message is reconstructed from a text projection. The final response remains
 * typed and owned by the caller; display text is projected only at consumers. */
int agent_request_with_tools_supported(const aimee_request_t *request);
int agent_execute_request_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *role, const aimee_request_t *request,
                                     aimee_response_t *response, agent_result_t *out);
int agent_run_request_with_tools(agent_config_t *config, const agent_t *selected,
                                 const aimee_request_t *request, aimee_response_t *response,
                                 agent_result_t *out);

int agent_run(agent_config_t *cfg, const char *role, const aimee_request_t *request,
              agent_result_t *out);

/* Source-bound native delegate entry. Keeps selected role and explicit host
 * no-tools policy; the admitted request is never flattened into a CLI prompt. */
int agent_run_native_request_for_role(agent_config_t *config, const agent_t *selected,
                                      const char *role, int tools, const aimee_request_t *request,
                                      aimee_response_t *response, agent_result_t *out);
int agent_native_request_tools_disabled(void);
int agent_native_request_turn_limit(void);

/* Per-thread override of agent_run tool-mode inference. agent_run
 * normally derives use_tools from the routed agent's config (tools_enabled +
 * exec-role); a delegate that decided tools-OFF (CLI --no-tools -> force_tools=0)
 * calls agent_run, so that inference would re-enable tools and ignore the
 * decision. The delegate no-tools path sets this to 1 around its agent_run call
 * to force use_tools=0 for the turn, then clears it. Scoped per-thread so
 * agent_run's other ~28 callers are unaffected. */
void agent_run_force_no_tools(int on);

/* Per-thread: make agent_dispatch_one's admission acquire FAIL FAST (at-limit) instead of
 * blocking, so a fan-out / fallback caller tries a different agent rather than queueing on
 * a busy one. Default off = block+queue (correct for a pinned single-agent turn). */
void agent_dispatch_set_fail_fast(int on);

/* Run one task on a specifically named configured agent (clones the agent_t
 * before mutation; a missing/disabled agent is a failed participant, no silent
 * fallback). Defined in agent_runtime.c beside the static helpers it uses,
 * called from agent_parallel.c's fan-out. */
int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const aimee_request_t *request, agent_result_t *out);

/* agent_run_named, but WITH tools. Exists for review panelists: they must route by
 * NAME (each lens is bound to a seat model) yet still reach aimee, and the two
 * existing tools-enabled runners select by role only.
 *
 * Panelists were tool-less, which is why a diff-only reviewer could not answer
 * "does anything call this?" or "is this file product?" — questions that decide
 * whether a change is real. With tools, the role's toolset applies: `review` maps
 * to `review_indexed` (code_search / find_symbol / search_memory / search_docs),
 * i.e. the diff plus aimee's own code + memory tooling, and no filesystem or write
 * tools. write_capable is pinned 0 regardless of the agent's config. */
int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
                               const aimee_request_t *request, agent_result_t *out);

/* Thread-local invocation policy used by parallel task workers. It affects only
 * the next in-thread named tool run and must be cleared by the caller. */
void agent_run_require_initial_tool_call(int on);

/* One-shot, TOOL-FREE text generation for UI drafting: selects a single non-CLI
 * (HTTP-provider) agent (prefer `agent_name`, else default, else first enabled
 * non-CLI) and runs the plain canonical completion path — no tools, no worktree,
 * no exec role. Returns the complete canonical text response. 0 on success. */
int agent_generate(agent_config_t *cfg, const char *agent_name, const aimee_request_t *request,
                   agent_result_t *out);
int agent_generate_request(agent_config_t *cfg, const aimee_request_t *request,
                           agent_result_t *out);

/* Like agent_run but forces tool execution regardless of agent config */
int agent_run_with_tools(agent_config_t *cfg, const char *role, const aimee_request_t *request,
                         agent_result_t *out);

/* Variant for delegates whose prompt explicitly says no edits should be made.
 * When enforce_writes is 0, write-role delegates still get tools but the
 * no-write watchdog is disabled for this run. */
int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const aimee_request_t *request, int enforce_writes,
                                       agent_result_t *out);

/* Fan out `task_count` tasks across worker threads and join.
 * deadline_ms > 0 cancels unfinished workers at the deadline and joins them.
 * Task requests remain owned by the producer until every worker has joined.
 * deadline_ms <= 0 keeps the unbounded join. Returns the number of successful results. */
int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out, int deadline_ms);

/* Delegate fallback helpers */
int agent_error_is_retryable(const char *error);
/* Should a fallback/retry caller try a different agent for this result? True for a
 * saturation refusal (AGENT_RC_AT_LIMIT) or a retryable provider error; false for
 * success (0) or a non-retryable hard failure. */
int agent_rc_should_try_another(int rc, const char *error);
/* Remaining milliseconds in the one request-wide delegate tool-loop budget.
 * Returns 0 when no request cap exists and -1 after an existing cap expires. */
int agent_request_tool_loop_remaining_ms(const agent_config_t *cfg);
int agent_try_same_tier_fallback(agent_config_t *cfg, agent_t **current, const char *role,
                                 const aimee_request_t *request, int enforce_writes,
                                 agent_result_t *out, int rc);

/* AGENT_RC_AT_LIMIT: agent_dispatch_one refused because the agent is already at
 * its max_parallel ceiling — a saturation signal DISTINCT from a run failure, so
 * a fan-out/retry caller tries a different agent and it is never recorded as a
 * provider-health fault. */
#define AGENT_RC_AT_LIMIT (-2)

/* Context assembly refused before the next provider request. Terminal for
 * routing/fallback and not a provider-health failure. */
#define AGENT_RC_CONTEXT_REFUSED (-3)

/* Durable capture failed. Execution may already have run tools, so retrying on
 * another provider or credential is forbidden and must not affect health. */
#define AGENT_RC_CAPTURE_FAILED (-4)
static inline int agent_rc_is_local_refusal(int rc)
{
   return rc == AGENT_RC_CONTEXT_REFUSED || rc == AGENT_RC_CAPTURE_FAILED;
}

/* THE single per-agent turn executor: the one place that enforces the per-agent
 * max_parallel concurrency cap and records provider-health for a model turn.
 * Acquires the agent's slot, runs the turn (use_tools -> the tool loop
 * agent_execute_with_tools_for_role, else a plain completion agent_execute),
 * releases, and records success/failure health. Returns 0 on success,
 * AGENT_RC_AT_LIMIT at its ceiling or AGENT_RC_CONTEXT_REFUSED on a context
 * refusal (neither records a provider health failure), or -1 on
 * a run failure (health recorded). Agent RESOLUTION (by name/role/route),
 * retry/fallback loops, write_capable, and outcome/feedback/cache logging stay in
 * the callers — only the model turn itself goes through here.
 * PRECONDITION: `net` must be non-NULL when use_tools != 0 (the tools executor
 * dereferences it); a plain completion (use_tools == 0) ignores `net`.
 * This is an EXTERNAL model-turn boundary — do NOT call it from a helper that
 * already runs inside another agent_dispatch_one for the same agent, or the inner
 * call re-acquires the same max_parallel slot (self-throttle). Such composite
 * helpers dispatch canonical tool-free requests directly. */
int agent_dispatch_one(const agent_t *ag, const agent_network_t *net, const char *role,
                       const aimee_request_t *request, int use_tools, agent_result_t *out);

/* Logging */
void agent_log_call(const agent_result_t *result, const char *role);
/* Persist one model call's normalised usage + cost to token_audit, billed
 * against the served model and tagged with the given turn-origin source (e.g.
 * "agent", "openai-ingress"). agent_log_call calls this for the internal agent
 * path; ingress handlers that run a provider call directly (and so never reach
 * agent_log_call) call it to record their spend. */
void agent_record_token_audit(const agent_result_t *result, const char *role, const char *source);
/* As agent_record_token_audit, but stamps an explicit usage_kind: "realized"
 * (provider-reported, the billable default), "partial" (observed usage from a
 * stream that aborted before completion), or "estimated" (no provider usage).
 * Lets streaming ingress preserve observed tokens as partial spend rather than
 * dropping them when the provider stream errors mid-flight. */
void agent_record_token_audit_kind(const agent_result_t *result, const char *role,
                                   const char *source, const char *usage_kind);
/* Build a token-audit row for a provider ingress path from raw usage counts and
 * record it, folding the memset+snprintf+field-copy boilerplate the provider
 * ingress handlers otherwise repeat per call site. requested_model/stop_reason
 * tolerate NULL (stored as ""). kind==NULL records via agent_record_token_audit
 * (i.e. "realized"); a non-NULL kind ("realized"/"partial"/...) routes through
 * agent_record_token_audit_kind. The caller owns the enable/threshold guard. */
void agent_ingress_record_cost(const char *agent_name, const char *agent_model,
                               const char *requested_model, const char *stop_reason,
                               int prompt_tokens, int completion_tokens, int cache_write_tokens,
                               int cache_read_tokens, const char *source, const char *kind);
/* Master gate (proposal §2/§7 rollout knob): returns 1 when
 * ingress_usage_accounting_enabled is set. The stateless ingress handlers call
 * this before writing their cost rows so ingress accounting can be flipped on
 * deliberately; internal agent accounting (agent_log_call) is unaffected. */
int agent_ingress_accounting_enabled(void);
/* Async-audit writer (ingress_audit_async rollout knob; DB1 write-concurrency
 * acceptance). enqueue_row hands a copy of `row` (a const db1_token_audit_row_t*,
 * opaque here to keep the db1 type out of this header) to the background writer so
 * the request thread is not blocked on the DB insert — returns 0 when enqueued,
 * -1 when the bounded ring is full (caller inserts inline, never dropping a row).
 * flush blocks until the queue has fully drained (clean shutdown + load-test
 * validation of drop rate). */
int agent_audit_async_enqueue_row(const void *row);
void agent_audit_async_flush(void);
/* Set a per-thread ingress source that overrides the source on any token_audit
 * row written on this thread (incl. via agent_log_call). Used by ingress workers
 * (e.g. /v1/runs) so spend driven through the agent loop is attributed to the
 * ingress origin. Pass "" to clear. */
void agent_set_ingress_source(const char *source);
int agent_get_stats(const char *name, agent_stats_t *out, int max);

/* Task type classification */

/* Resolve the effective max-turns limit for an agent invocation.
 * Primary sessions (role == NULL) ignore agent->max_turns; only delegates cap. */
int agent_resolve_max_turns(const agent_t *agent, const char *role);

/* Map a routing role to the role a run is BUDGETED as: NULL (i.e. "primary,
 * uncapped") for a primary turn, otherwise `role` unchanged. See the definition
 * in posix/agent_max_turns.c. */
const char *agent_budget_role(const char *role);
const char *task_type_name(task_type_t type);

/* Context assembly */
size_t agent_exec_context_budget_chars(const agent_t *agent);
/* Opening instruction for a task type. A review must be told to answer; an
 * execution agent must be told to act. Pure so the choice is testable. */
/* The task a delegate is actually running. A declared role wins over guessing
 * from prose, which cannot be trusted for a review: the panel prompt contains
 * bug-fix vocabulary and the classifier scans its keyword table in order. */
task_type_t agent_task_type_for_role(const char *role);

const char *agent_exec_instructions(task_type_t task_type);

/* Owned canonical context origin, ordered source blocks. Policy and evidence
 * authority are recorded at generation; consumers must preserve that split.
 * Failure leaves out unchanged and retains the owner diagnostic. */
int agent_build_exec_context_ir_checked(const agent_t *agent, const agent_network_t *network,
                                        const char *role, const char *custom_prompt,
                                        int skip_kb_context, aimee_message_t *out, char *error,
                                        size_t error_len);

/* Ephemeral SSH */
int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len);
void agent_ssh_cleanup(const agent_network_t *network, const char *key_path,
                       const char *session_id);

/* Policy, trace, confidence */
/* Validate against the exact admitted IR function schema. Optional registry
 * enable/disable state remains policy; registry/provider schemas cannot replace
 * the advertised contract. Arguments are the same canonical bytes as dispatch. */
int tool_validate_ir(const aimee_tool_t *tool, const char *args_json, char *err_out,
                     size_t err_len);
const char *tool_suggest(const char *name);
void agent_trace_log(int plan_id, int turn, const char *direction, const char *content,
                     const char *tool_name, const char *tool_args, const char *tool_result,
                     const char *context_hash);
int agent_estimate_confidence(const char *response_text);
int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len);
int policy_check_tool_attempt(const char *tool_name, const char *side_effect, const char *args_json,
                              const char *attempt_id, char *reason_out, size_t reason_len);
int tool_policy_metadata(const char *tool, aimee_tool_metadata_t *metadata, char *effect,
                         size_t effect_len, char *reason, size_t reason_len);
int policy_check_tool_metadata_attempt(const char *tool, const aimee_tool_metadata_t *metadata,
                                       const char *effect, const char *arguments,
                                       const char *attempt, char *reason, size_t reason_len);

int policy_check_session_tool(const char *session, const char *tool, const char *arguments,
                              const char *attempt, char *reason, size_t reason_len);
char *policy_observe_indexed(const char *tool, const char *arguments, const char *attempt,
                             const char *result);
char *policy_expand_exploration(const char *reason, const char *gap, const char *outcome);
void policy_complete_exploration_turn(int turn);
int policy_bind_session_exploration(const char *session);
char *policy_annotate_indexed(const char *tool, const char *arguments, const char *attempt,
                              char *result);
int policy_load(void);
typedef struct policy_action_scope policy_action_scope_t;
/* Lexically suspend the current final-action owner for synchronous nested
 * producer work. Pending JSON is moved, not copied or authorized anew. */
int policy_action_scope_push(policy_action_scope_t **out);
int policy_action_scope_pop(policy_action_scope_t *scope);
void policy_action_attempt(const char *attempt);
int policy_action_inherit(const char *session);
int policy_recheck_action_tool_attempt(const char *tool, const char *effect, const char *arguments,
                                       const char *attempt, uint32_t resource_categories,
                                       char *reason, size_t reason_len);
int policy_action_begin(const char *name, struct cJSON **args, const char *cwd, const char *sid,
                        int authorized);
char *policy_action_finish(const char *verdict, char *result, int *is_error);
struct cJSON;
int policy_prepare_exploration(const struct cJSON *offer, const char *session,
                               const char *workspace, const char *project);

/* Metrics, introspection, manifests, contract */
void agent_write_metrics(void);
void agent_write_manifest(const char *run_id, const agent_result_t *result, const char *role);
char *agent_load_project_contract(const char *project_root);

/* Pure clamp half of agent_tool_output_cap(): map a configured
 * tool_output_max_bytes value to an effective per-result cap. 0/negative ->
 * AGENT_TOOL_OUTPUT_MAX (built-in default); any positive value is clamped to
 * (0, AGENT_TOOL_OUTPUT_RAW_MAX]. Header-inline so it is the single source of
 * truth for both the config-reading resolver and unit tests (zero linkage). */
static inline size_t agent_tool_output_cap_clamp(int configured)
{
   if (configured <= 0)
      return (size_t)AGENT_TOOL_OUTPUT_MAX;
   if ((size_t)configured > (size_t)AGENT_TOOL_OUTPUT_RAW_MAX)
      return (size_t)AGENT_TOOL_OUTPUT_RAW_MAX;
   return (size_t)configured;
}

/* Resolve the per-result MODEL-VISIBLE tool-output cap (bytes). Reads
 * tool_output_max_bytes from config (mtime-cached) and clamps via
 * agent_tool_output_cap_clamp(). Single source of truth for every
 * model-visible tool-result truncation site. */
size_t agent_tool_output_cap(void);

/* Provider health */
typedef enum
{
   PROVIDER_ERR_NONE = 0,
   PROVIDER_ERR_NETWORK,    /* connection refused, DNS failure, timeout */
   PROVIDER_ERR_AUTH,       /* 401, 403 */
   PROVIDER_ERR_RATE_LIMIT, /* 429 */
   PROVIDER_ERR_SERVER,     /* 5xx */
   PROVIDER_ERR_CLIENT,     /* other 4xx */
   PROVIDER_ERR_UNKNOWN
} provider_err_class_t;

provider_err_class_t provider_classify_error(int http_status);
const char *provider_error_message(provider_err_class_t cls);
/* HTTP */

/* Upper bound on the TCP-connect phase, independent of the overall request
 * timeout. A reachable host completes the TCP handshake in milliseconds, so a
 * connect that takes seconds means the host is unreachable — e.g. a down
 * dependency behind a gateway that silently drops SYNs (no RST). Without this
 * cap an unreachable host blocks for the FULL request timeout (e.g. the 60s kb
 * action timeout), so reads like /v1/memory/recall and /v1/memory/stats hang
 * instead of fast-failing the way /v1/kb/status (5s) does. The longer request
 * timeout still governs the response read for hosts that are up but slow. */
#define AGENT_HTTP_CONNECT_TIMEOUT_MS 5000

/* Effective connect-phase poll timeout for a request whose overall budget is
 * request_timeout_ms: min(budget-or-30s-default, AGENT_HTTP_CONNECT_TIMEOUT_MS).
 * Inline so the HTTP client and its tests share one definition. */
static inline int agent_http_effective_connect_timeout_ms(int request_timeout_ms)
{
   int connect_ms = request_timeout_ms > 0 ? request_timeout_ms : 30000;
   return connect_ms > AGENT_HTTP_CONNECT_TIMEOUT_MS ? AGENT_HTTP_CONNECT_TIMEOUT_MS : connect_ms;
}

int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms);
/* GET that REPORTS a 3xx redirect target in `location` rather than following it.
 * Deliberately non-following: a forge's log endpoint redirects to pre-signed
 * third-party storage, and replaying the request there would hand that host the
 * Authorization header. Only the caller knows which of its headers are
 * host-bound, so the caller issues the second request itself. */
int agent_http_get_location(const char *url, const char *extra_headers, char *location,
                            size_t location_cap, char **response_buf, int timeout_ms);
/* SSRF-safe GET: connect to the caller-validated numeric `pinned_ip` (no DNS
 * re-resolution) with Host/SNI still taken from the URL host. */
int agent_http_get_pinned(const char *url, const char *pinned_ip, const char *extra_headers,
                          char **response_buf, int timeout_ms);
int agent_http_put(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers);
int agent_http_patch(const char *url, const char *auth_header, const char *body,
                     char **response_buf, int timeout_ms, const char *extra_headers);
int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers);
/* Exact-length JSON POST used by immutable provider-wire snapshots. Unlike the
 * compatibility wrapper above, this never recomputes the body boundary. */
int agent_http_post_bytes(const char *url, const char *auth_header, const void *body,
                          size_t body_len, char **response_buf, int timeout_ms,
                          const char *extra_headers);
/* Per-call send guard. Acquisition runs after connection setup and request
 * construction, immediately before the first request write. Release runs on
 * both refusal and write completion, before reading a provider response. Nested
 * ordinary HTTP requests never inherit this guard. */
typedef struct agent_http_send_guard
{
   void *context;
   int (*acquire)(void *context);
   void (*release)(void *context, int write_status);
   /* Optional acquisition + write budget, starting before acquire. The
    * response retains the caller's original request deadline. */
   int send_timeout_ms;
} agent_http_send_guard_t;
int agent_http_post_guarded_bytes(const char *url, const char *auth_header, const void *body,
                                  size_t body_len, char **response_buf, int timeout_ms,
                                  const char *extra_headers, const agent_http_send_guard_t *guard);
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers);
/* Binary-safe content-type POST. Both wire boundaries are explicit lengths;
 * the owned response also has a trailing NUL for compatibility, outside its
 * reported length. On failure the response is NULL and its length is zero. */
int agent_http_post_content_type_bytes(const char *url, const char *auth_header,
                                       const char *content_type, const void *body, size_t body_len,
                                       char **response_buf, size_t *response_len, int timeout_ms,
                                       const char *extra_headers);

/* Retry-After (seconds) parsed from the most recent buffered HTTP response on
 * this thread, or 0 if it carried none. Lets the Anthropic ingress relay an
 * upstream 429/529 Retry-After to the client for exact parity. */
int agent_http_last_retry_after(void);

/* Streaming callback: called for each data chunk. Return 0 to continue, non-zero to abort. */
typedef int (*agent_http_stream_cb)(const char *data, size_t len, void *userdata);

int agent_http_get_stream(const char *url, const char *extra_headers, agent_http_stream_cb callback,
                          void *userdata, int timeout_ms);
int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers);
int agent_http_post_stream_bytes(const char *url, const char *auth_header, const void *body,
                                 size_t body_len, agent_http_stream_cb callback, void *userdata,
                                 int timeout_ms, const char *extra_headers);
int agent_http_post_stream_guarded_bytes(const char *url, const char *auth_header, const void *body,
                                         size_t body_len, agent_http_stream_cb callback,
                                         void *userdata, int timeout_ms, const char *extra_headers,
                                         const agent_http_send_guard_t *guard);
void agent_http_init(void);
void agent_http_cleanup(void);

/* Gemini Prompt Cache
 *
 * Gemini supports explicit caching of system prompts ("cachedContent").
 * Create once before the agent loop; attach to each request; delete on cleanup.
 * All functions degrade silently on error so callers need no special handling. */

/* Try to create a cached-content resource for system_prompt.
 * On success fills cache_name_out and returns 0; returns -1 on any error.
 * endpoint_base: e.g. "https://generativelanguage.googleapis.com/v1beta" (NULL = default)
 * timeout_ms: per-request HTTP timeout */

/* Attach a cached-content reference to an already-built Gemini request body.
 * Removes "systemInstruction" (it lives in the cache) and adds "cachedContent". */

/* Delete a cached-content resource. Errors are logged at DEBUG, never surfaced. */

#endif /* DEC_AGENT_EXEC_H */

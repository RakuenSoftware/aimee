#ifndef DEC_AGENT_EXEC_H
#define DEC_AGENT_EXEC_H 1

#include "agent_types.h"

struct cJSON; /* forward declaration for gemini_prompt_cache_attach */

/* Execution */
int agent_execute_with_tools(const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out);

int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature,
                                      agent_result_t *out);

int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system_prompt, const char *user_prompt,
                                     int max_tokens, double temperature,
                                     struct cJSON *initial_messages,
                                     struct cJSON **updated_messages, agent_result_t *out);

int agent_execute(const agent_t *agent, const char *system_prompt, const char *user_prompt,
                  int max_tokens, double temperature, agent_result_t *out);

int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out);

/* Like agent_run, but with an explicit sampling temperature. agent_run is the
 * thin wrapper that passes the historical 0.3 default, so its ~28 call sites are
 * byte-unchanged; the parallel fan-out path uses this to honour a per-task
 * temperature (agent_task_t.temperature), which agent_run dropped. */
int agent_run_ex(agent_config_t *cfg, const char *role, const char *system_prompt,
                 const char *user_prompt, int max_tokens, double temperature, agent_result_t *out);

/* Run one task on a specifically named configured agent (clones the agent_t
 * before mutation; a missing/disabled agent is a failed participant, no silent
 * fallback). Defined in agent_runtime.c beside the static helpers it uses,
 * called from agent_parallel.c's fan-out. */
int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const char *system_prompt, const char *user_prompt, int max_tokens,
                    double temperature, agent_result_t *out);

/* One-shot, TOOL-FREE text generation for UI drafting: selects a single non-CLI
 * (HTTP-provider) agent (prefer `agent_name`, else default, else first enabled
 * non-CLI) and runs the plain-completion agent_execute() — no tools, no worktree,
 * no exec role. The model can only return text (out->response). 0 on success. */
int agent_generate(agent_config_t *cfg, const char *agent_name, const char *system_prompt,
                   const char *user_prompt, int max_tokens, double temperature,
                   agent_result_t *out);

/* Like agent_run but forces tool execution regardless of agent config */
int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *system_prompt,
                         const char *user_prompt, int max_tokens, agent_result_t *out);

/* Variant for delegates whose prompt explicitly says no edits should be made.
 * When enforce_writes is 0, write-role delegates still get tools but the
 * no-write watchdog is disabled for this run. */
int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const char *system_prompt, const char *user_prompt,
                                       int max_tokens, int enforce_writes, agent_result_t *out);

/* Fan out `task_count` tasks across worker threads and join.
 * deadline_ms > 0 bounds the wait: a worker still running at the deadline is
 * abandoned (detached) and its slot in `out` left as a failed (zeroed) result,
 * so one hung model can never wedge the whole fan-out. deadline_ms <= 0 keeps the
 * historical unbounded join. Returns the number of successful results. */
int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out, int deadline_ms);

/* Delegate fallback helpers */
int agent_error_is_retryable(const char *error);
int agent_try_same_tier_fallback(agent_config_t *cfg, agent_t **current, const char *role,
                                 const char *system_prompt, const char *user_prompt, int max_tokens,
                                 int enforce_writes, agent_result_t *out, int rc);

/* AGENT_RC_AT_LIMIT: agent_dispatch_one refused because the agent is already at
 * its max_parallel ceiling — a saturation signal DISTINCT from a run failure, so
 * a fan-out/retry caller tries a different agent and it is never recorded as a
 * provider-health fault. */
#define AGENT_RC_AT_LIMIT (-2)

/* THE single per-agent turn executor: the one place that enforces the per-agent
 * max_parallel concurrency cap and records provider-health for a model turn.
 * Acquires the agent's slot, runs the turn (use_tools -> the tool loop
 * agent_execute_with_tools_for_role, else a plain completion agent_execute),
 * releases, and records success/failure health. Returns 0 on success,
 * AGENT_RC_AT_LIMIT if the agent was at its ceiling (NO health recorded), or -1 on
 * a run failure (health recorded). Agent RESOLUTION (by name/role/route),
 * retry/fallback loops, write_capable, and outcome/feedback/cache logging stay in
 * the callers — only the model turn itself goes through here. */
int agent_dispatch_one(const agent_t *ag, const agent_network_t *net, const char *role,
                       const char *system_prompt, const char *user_prompt, int max_tokens,
                       double temperature, int use_tools, agent_result_t *out);

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
/* Record a context-economizer ledger row (usage_kind="avoided", FORECAST-only —
 * see context_reduce.h). Forward-declared struct so this broad header need not
 * pull in context_reduce.h; the caller includes it for the full type. */
struct reduce_result_s;
void agent_record_reduce_ledger(const struct reduce_result_s *r, const char *model,
                                const char *agent_name, const char *role);
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
task_type_t task_type_classify(const char *prompt);

/* Resolve the effective max-turns limit for an agent invocation.
 * Primary sessions (role == NULL) ignore agent->max_turns; only delegates cap. */
int agent_resolve_max_turns(const agent_t *agent, const char *role);
const char *task_type_name(task_type_t type);

/* Context assembly */
size_t agent_exec_context_budget_chars(const agent_t *agent);
char *agent_build_exec_context(const agent_t *agent, const agent_network_t *network,
                               const char *custom_prompt);
char *agent_build_exec_context_ex(const agent_t *agent, const agent_network_t *network,
                                  const char *custom_prompt, int skip_kb_context);
void agent_print_context(const agent_config_t *cfg);

/* Ephemeral SSH */
int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len);
void agent_ssh_cleanup(const agent_network_t *network, const char *key_path,
                       const char *session_id);

/* Policy, trace, confidence */
int tool_validate(const char *tool_name, const char *args_json, char *err_out, size_t err_len);
const char *tool_suggest(const char *name);
const char *tool_side_effect(const char *tool_name);
char *agent_collect_tool_prompts(void);
void agent_trace_log(int plan_id, int turn, const char *direction, const char *content,
                     const char *tool_name, const char *tool_args, const char *tool_result,
                     const char *context_hash);
int agent_estimate_confidence(const char *response_text);
int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len);
int policy_load(void);

/* Metrics, introspection, manifests, contract */
void agent_write_metrics(void);
void agent_introspect_env(void);
void agent_write_manifest(const char *run_id, const agent_result_t *result, const char *role);
char *agent_load_project_contract(const char *project_root);
/* Compact a raw tool result string using application config.
 * tool_name may be NULL to skip per-tool overrides. Returns a heap-allocated
 * string bounded by AGENT_TOOL_OUTPUT_MAX; caller must free(). */
char *agent_compress_tool_result(const char *raw, size_t raw_len, const char *tool_name);

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

/* Self-correcting delegate loop
 *
 * Wraps agent_run in a quality-checking loop. After each iteration the agent
 * is asked to self-assess task completion (0-100). Execution continues until
 * the completion score meets the threshold, the optional verify command passes,
 * or the iteration cap is reached.
 *
 * This is separate from --retry (transient failure retry). The loop addresses
 * quality: "task not fully done", whereas retry addresses reliability: "API
 * error, try again". */

#define AGENT_LOOP_MAX_ITER_DEFAULT  5    /* maximum iterations before giving up */
#define AGENT_LOOP_THRESHOLD_DEFAULT 95   /* stop when completion >= this */
#define AGENT_LOOP_CONTEXT_MAX       8192 /* max bytes of accumulated context */

typedef struct
{
   int max_iterations;       /* cap on total iterations */
   int completion_threshold; /* 0-100: stop when self-assessed >= this */
   char verify_cmd[1024];    /* optional shell command; failure → continue loop */
   /* runtime (zeroed on init) */
   int current_iteration;
   int last_completion;       /* last parsed completion score */
   char *accumulated_context; /* heap: summary of prior iterations (NULL if none) */
} agent_loop_t;

/* Initialise loop with explicit parameters. Sets runtime fields to zero. */
void agent_loop_init(agent_loop_t *loop, int max_iter, int threshold, const char *verify_cmd);

/* Free heap resources. Safe to call on a zero-initialised struct. */
void agent_loop_free(agent_loop_t *loop);

/* Parse a completion score and optional gaps from a delegate response.
 * Scans for the last occurrence of {"completion": N, "gaps": [...]} in text.
 * Returns the completion score (0-100) on success, or -1 if not found.
 * If gaps_out is non-NULL and gaps_len > 0, fills it with the "gaps" array
 * as a comma-separated string. */
int agent_loop_parse_completion(const char *response, char *gaps_out, size_t gaps_len);

/* Run a self-correcting agent loop.
 * Executes agent_run repeatedly, augmenting the prompt with self-assessment
 * instructions and feeding prior-iteration context into each continuation.
 * Stops when:
 *   (a) self-assessed completion >= loop->completion_threshold AND
 *       verify_cmd passes (or is empty), OR
 *   (b) loop->max_iterations is exhausted.
 * out->response is set to the final iteration's response (caller must free).
 * Returns 0 when the task is considered complete, -1 on hard failure. */
int agent_loop_run(agent_config_t *cfg, const char *role, const char *system_prompt,
                   const char *original_prompt, int max_tokens, agent_loop_t *loop,
                   agent_result_t *out);

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

typedef struct
{
   int available;         /* 1=ok, 0=unreachable, -1=unknown */
   int64_t last_check_ms; /* monotonic timestamp of last check */
   int last_http_status;  /* last HTTP status code, or -1 for network error */
   char error[256];       /* last error message */
} provider_health_t;

provider_err_class_t provider_classify_error(int http_status);
const char *provider_error_message(provider_err_class_t cls);
void provider_health_update(const char *provider_name, int http_status);
const provider_health_t *provider_health_get(const char *provider_name);

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
int agent_http_put(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers);
int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers);
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers);
int agent_http_delete(const char *url, const char *auth_header, int timeout_ms);

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
int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms);
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
int gemini_prompt_cache_create(const char *endpoint_base, const char *auth_header,
                               const char *model, const char *system_prompt, int timeout_ms,
                               char *cache_name_out, size_t name_len);

/* Attach a cached-content reference to an already-built Gemini request body.
 * Removes "systemInstruction" (it lives in the cache) and adds "cachedContent". */
void gemini_prompt_cache_attach(struct cJSON *req, const char *cache_name);

/* Delete a cached-content resource. Errors are logged at DEBUG, never surfaced. */
void gemini_prompt_cache_delete(const char *endpoint_base, const char *auth_header,
                                const char *cache_name, int timeout_ms);

#endif /* DEC_AGENT_EXEC_H */

/* posix/agent_runtime.c: POSIX execution engine — agent loop, context assembly, and eval */
/* ================================================================
 * From: agent.c
 * ================================================================ */
/* posix/agent.c: POSIX implementation of agent_execute_with_tools. */
#include "aimee.h"
#include "agent.h"
#include "aimee_ir_shadow.h"
#include "aimee_backend.h"  /* anthropic_backend_parse / openai_backend_parse */
#include "aimee_ir.h"       /* aimee_response_t + block accessors */
#include "tool_call_args.h" /* assistant_message arg normalize/sanitize */
#include "agent_exec.h"
#include "agent_protocol.h"
#include "agent_runtime_messages.h"
#include "agent_tools.h"
#include "agent_tunnel.h"
#include "delegate_driver.h"
#include "delegate_role.h"
#include "delegate_xml_fallback.h"
#include "gateway_delegate.h"
#include "gateway_policy.h"
#include "http_retry.h"
#include "log.h"
#include "middleware.h"
#include "model_registry.h"
#include "payload_rewrite.h"
#include "util.h"
#include "rounds_to_resume.h"
#include "session_compact.h"
#include "liveness.h"
#include "provider_cli_adapter.h"
#include "config.h"
#include "economizer.h"
#include "fold_recall.h"
#include "dstr.h"
#include "cJSON.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static void finish_final_tool_violation(agent_result_t *out, const agent_t *agent,
                                        const char *attempted_tool, const char *partial_text,
                                        const char *last_tool_name, const char *last_tool_result,
                                        int total_tool_calls)
{
   char diag[1800];
   liveness_format_final_tool_call_diagnostic(agent ? agent->name : NULL, attempted_tool,
                                              partial_text, last_tool_name, last_tool_result,
                                              total_tool_calls, diag, sizeof(diag));

   free(out->response);
   out->response = safe_strdup(diag);
   out->success = 0;
   out->abstained = 1;
   snprintf(out->abstain_reason, sizeof(out->abstain_reason),
            "final response synthesized after disabled tool call");
   snprintf(out->error, sizeof(out->error), "model attempted %s on the forced final response turn",
            attempted_tool && attempted_tool[0] ? attempted_tool : "a tool call");
}

/* agent_store_feedback is defined in agent.c and shared across platforms */
void agent_store_feedback(const agent_result_t *result, const char *role,
                          const char *prompt_summary);

/* db1 agent_jobs heartbeat — defined in src/db1/agent_jobs.c. Forward
 * declarations match db1/agent_jobs.h to avoid pulling the db1 umbrella
 * header through the runtime's already-large include set. */
void db1_agent_job_update(int job_id, const char *status, int cursor_turn, const char *result);
void db1_agent_job_heartbeat(int job_id);
void db1_agent_job_heartbeat_ext(int job_id, const char *current_tool, int api_call_count);
int db1_agent_job_is_cancelled(int job_id);
int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz);
const char *delegation_active_id(void);

static int agent_delegation_stopped(char *buf, size_t bufsz)
{
   const char *delegation_id = delegation_active_id();
   if (!delegation_id || !delegation_id[0])
      return 0;

   char reason[32];
   int stopped = db1_delegation_spawn_stop_reason(delegation_id, reason, sizeof(reason));
   if (stopped != 1)
      return 0;
   if (buf && bufsz > 0)
      snprintf(buf, bufsz, "delegate %s (%s)", reason, delegation_id);
   return 1;
}

static int agent_durable_cancelled(char *buf, size_t bufsz)
{
   int job_id = agent_get_durable_job_id();
   if (job_id > 0 && db1_agent_job_is_cancelled(job_id))
   {
      if (buf && bufsz > 0)
         snprintf(buf, bufsz, "delegate cancelled (job #%d)", job_id);
      return 1;
   }
   return agent_delegation_stopped(buf, bufsz);
}

static int is_chatgpt_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "chatgpt") == 0;
}

static int is_anthropic_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "anthropic") == 0;
}

static int agent_allows_json_content_rescue(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "llama_native") == 0 || strcmp(agent->provider, "ollama") == 0 ||
       strcmp(agent->provider, "llama-eval") == 0)
      return 1;
   if (strstr(agent->name, "llama") || strstr(agent->model, ".gguf") ||
       strstr(agent->model, "Mistral") || strstr(agent->model, "mistral") ||
       strstr(agent->model, "Ministral") || strstr(agent->model, "ministral") ||
       strstr(agent->model, "Devstral") || strstr(agent->model, "devstral"))
      return 1;
   return 0;
}

static int agent_should_inject_respond_tool(const agent_t *agent, const char *role)
{
   if (!agent || !role || !role[0])
      return 0;
   if (agent->ablation.configured && !agent->ablation.respond_tool)
      return 0;
   if (!agent->tools_enabled || !agent->inject_respond_tool)
      return 0;
   if (is_chatgpt_provider(agent) || is_anthropic_provider(agent))
      return 0;
   return 1;
}

static int request_prompt_token_estimate(cJSON *messages, const char *system_prompt)
{
   int tokens = session_compact_estimate_tokens(messages);
   if (system_prompt && system_prompt[0])
      tokens += (int)(strlen(system_prompt) / 4) + 1;
   return tokens;
}

static int agent_effective_context_window(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (agent->middleware.context_window > 0)
      return agent->middleware.context_window;
   return model_context_window(agent->model);
}

static int agent_effective_auto_compact_pct(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (agent->middleware.auto_compact_pct < 0)
      return 0;
   if (agent->middleware.auto_compact_pct > 0)
      return agent->middleware.auto_compact_pct;
   return SESSION_COMPACT_DEFAULT_COMPACT_PCT;
}

static void maybe_compact_before_request(const agent_t *agent, cJSON *messages,
                                         const char *system_prompt)
{
   int context_window = agent_effective_context_window(agent);
   int compact_pct = agent_effective_auto_compact_pct(agent);
   if (!messages || context_window <= 0 || compact_pct <= 0)
      return;

   session_compact_config_t scfg;
   memset(&scfg, 0, sizeof(scfg));
   scfg.compact_pct = compact_pct;

   int estimated_tokens = request_prompt_token_estimate(messages, system_prompt);
   if (session_compact_pressure(estimated_tokens, 0, context_window, &scfg) !=
       SESSION_PRESSURE_COMPACT)
      return;

   session_compact_result_t sc_result;
   if (session_compact(messages, &scfg, &sc_result) == 0 && sc_result.compacted)
      aimee_log(LOG_INFO, "agent",
                "session compacted before request: %d→%d messages (%d removed, %d repairs)",
                sc_result.messages_before, sc_result.messages_after, sc_result.messages_removed,
                sc_result.repairs);
   else
   {
      message_history_repair(messages);
      messages_compact_consecutive(messages);
   }
}

/* Text of the most-recent user message (string or first text block); NULL if none. */
static const char *newest_user_text(const cJSON *messages)
{
   int n = cJSON_GetArraySize((cJSON *)messages);
   for (int i = n - 1; i >= 0; i--)
   {
      cJSON *m = cJSON_GetArrayItem((cJSON *)messages, i);
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
      if (!role || strcmp(role, "user") != 0)
         continue;
      cJSON *c = cJSON_GetObjectItem(m, "content");
      if (cJSON_IsString(c))
         return c->valuestring;
      if (cJSON_IsArray(c))
      {
         cJSON *b;
         cJSON_ArrayForEach(b, c)
         {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
            if (t && strcmp(t, "text") == 0)
               return cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
         }
      }
      return NULL;
   }
   return NULL;
}

/* Add the recall-worthy coordinates (paths, handle:/memory: ids) of the folded
 * prefix messages[0..split) to the recall index for future re-touch detection. */
static void recall_index_from_fold(const cJSON *messages, int split, fold_recall_index_t *ix)
{
   coord_set_t set;
   coord_set_init(&set);
   coord_provenance_t prov = {COORD_LANE_AGENT, -1, -1, -1};
   for (int i = 0; i < split; i++)
   {
      cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
      if (!it)
         continue;
      char *s = cJSON_PrintUnformatted(it);
      if (!s)
         continue;
      coord_closet_nominate(s, strlen(s), &prov, &set);
      free(s);
   }
   for (size_t i = 0; i < set.count; i++)
      if (set.items[i].kind == COORD_KIND_PATH || set.items[i].kind == COORD_KIND_HANDLE)
         fold_recall_index_add(ix, set.items[i].value);
   coord_set_free(&set);
}

/* Fold §1/§3/§4 (P2b/P2c/P4): build a rolling-fold view of the Anthropic message
 * array if the fold is enabled. Populates *out (zeroed on no-fold). The fold's
 * Coordinate Closet is rendered during this call, so the config's denylist (a
 * pointer into the local config_t) only needs to be valid here — out holds owned
 * cJSON that the caller frees with fold_result_free AFTER the request is
 * serialized. `freeze` (may be NULL) is the per-run fold-freeze state honored only
 * when fold_freeze_enabled; `recall` (may be NULL) is the per-run recall index for
 * §4 re-touch hints, honored only when fold_recall_enabled. `turn` is the current
 * turn index (for recall residency). Returns 1 if a fold view was produced. */
static int build_fold_view(const cJSON *messages, fold_freeze_t *freeze,
                           fold_recall_index_t *recall, int turn, fold_result_t *out)
{
   memset(out, 0, sizeof(*out));
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.fold_enabled)
      return 0;
   fold_config_t fc;
   memset(&fc, 0, sizeof(fc));
   fc.enabled = 1;
   fc.retained_msgs = cfg.fold_retained_msgs;
   fc.min_fold_msgs = cfg.fold_min_fold_msgs;
   fc.reasoning_excerpt_bytes = cfg.fold_excerpt_bytes;
   fc.register_enabled = cfg.fold_register_enabled;
   fc.closet.enabled = cfg.coord_closet_enabled;
   fc.closet.budget_bytes = cfg.coord_closet_budget_bytes;
   fc.closet.max_ratio_pct = cfg.coord_closet_max_ratio_pct;
   fc.closet.denylist = cfg.coord_closet_denylist[0] ? cfg.coord_closet_denylist : NULL;
   fold_freeze_t *fz = NULL;
   if (cfg.fold_freeze_enabled && freeze)
   {
      if (freeze->tail_cap_msgs == 0 && cfg.fold_freeze_tail_cap_msgs > 0)
         freeze->tail_cap_msgs = cfg.fold_freeze_tail_cap_msgs;
      fz = freeze;
   }
   context_fold_view(messages, &fc, fz, out);
   if (!out->folded)
      return 0;
   aimee_log(LOG_DEBUG, "fold", "folded=%d retained=%d reused_boundary=%d epochs=%d",
             out->folded_msgs, out->retained_msgs, out->reused_boundary, fz ? fz->epochs : 0);

   /* §4 recall: detect re-touch of a previously-folded coordinate in this turn's
    * newest user message and surface a hint in the fold preamble; then index this
    * fold's coordinates for future turns. Detection precedes indexing so a
    * just-folded key does not trivially match the same turn. */
   if (cfg.fold_recall_enabled && recall)
   {
      const char *uq = newest_user_text(messages);
      dstr_t hints;
      dstr_init(&hints);
      size_t hit = uq ? fold_recall_detect(recall, uq, turn, cfg.fold_recall_ttl_turns, &hints) : 0;
      if (hit && out->messages)
      {
         cJSON *m0 = cJSON_GetArrayItem(out->messages, 0);
         const char *body = m0 ? cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")) : NULL;
         if (m0 && body)
         {
            dstr_t merged;
            dstr_init(&merged);
            dstr_appendf(&merged, "[fold recall — re-touched coordinates]\n%s\n%s",
                         dstr_cstr(&hints), body);
            cJSON *ns = cJSON_CreateString(dstr_cstr(&merged)); /* copies; merged freed below */
            if (ns) /* on OOM leave the original content intact rather than nulling it */
               cJSON_ReplaceItemInObjectCaseSensitive(m0, "content", ns);
            dstr_free(&merged);
         }
      }
      dstr_free(&hints);
      recall_index_from_fold(messages, out->folded_msgs, recall);
   }
   return out->folded;
}

static void track_anthropic_payload_rewrite(const delegate_driver_t *driver, const agent_t *agent,
                                            cJSON *messages, const char *system_prompt)
{
   driver_caps_t caps;
   delegate_get_caps(driver, agent, &caps);
   payload_rewrite_track_request(system_prompt, NULL,
                                 request_prompt_token_estimate(messages, system_prompt),
                                 caps.context_limit);
}

/* Returns 1 if payload_rewrite policy says to skip the context rebuild. */
static int anthropic_context_refresh_deferred(const delegate_driver_t *driver, const agent_t *agent,
                                              cJSON *messages, const char *sys)
{
   const char *sid = session_id();
   char hash[17];
   payload_rewrite_prefix_hash(sys, NULL, hash, sizeof(hash));
   driver_caps_t caps;
   delegate_get_caps(driver, agent, &caps);
   payload_rewrite_decision_t dec;
   return payload_rewrite_should_defer(sid ? sid : "", hash,
                                       request_prompt_token_estimate(messages, sys),
                                       caps.context_limit, &dec) == 0 &&
          dec.defer;
}

static const char *default_exec_instructions =
    "# Instructions\n"
    "- IMPORTANT: Always call tools to act. Never write shell commands as plain text.\n"
    "- Use the bash tool to run commands, including SSH to remote hosts.\n"
    "- For code discovery, use find_symbol, code_search, or aimee index commands before shell "
    "search. If a shell search is blocked by policy, follow the policy guidance and continue.\n"
    "- If find_symbol returns no results for a named artifact, always run rg in the exact "
    "worktree before reporting a symbol, file, or API as missing.\n"
    "- Aimee index, code_search, find_symbol, and search_memory are authoritative for discovery "
    "and prior context. Current source is authoritative for file contents: source packets, "
    "explicitly preloaded files, and read_file results from this delegate worktree override "
    "indexed snippets when they differ.\n"
    "- Use read_file to read, edit_file to change part of an existing file "
    "(old_string/new_string — you need not reproduce the whole file), and write_file to "
    "create a file or overwrite it wholesale.\n"
    "- Use list_files to explore directories.\n"
    "- When you have completed the task, respond with a final summary.\n"
    "- If you encounter an error, try to diagnose and fix it.\n"
    "- Do not ask for confirmation. Execute the task directly.\n";

int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out);

static int agent_execute_with_tools_internal(const agent_t *agent, const agent_network_t *network,
                                             const char *role, const char *system_prompt,
                                             const char *user_prompt, int max_tokens,
                                             double temperature, cJSON *initial_messages,
                                             cJSON **updated_messages, agent_result_t *out);

int agent_execute_with_tools(const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out)
{
   return agent_execute_with_tools_internal(agent, network, NULL, system_prompt, user_prompt,
                                            max_tokens, temperature, NULL, NULL, out);
}

int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature,
                                      agent_result_t *out)
{
   return agent_execute_with_tools_internal(agent, network, role, system_prompt, user_prompt,
                                            max_tokens, temperature, NULL, NULL, out);
}

int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system_prompt, const char *user_prompt,
                                     int max_tokens, double temperature, cJSON *initial_messages,
                                     cJSON **updated_messages, agent_result_t *out)
{
   return agent_execute_with_tools_internal(agent, network, NULL, system_prompt, user_prompt,
                                            max_tokens, temperature, initial_messages,
                                            updated_messages, out);
}

static int agent_execute_with_tools_internal(const agent_t *agent, const agent_network_t *network,
                                             const char *role, const char *system_prompt,
                                             const char *user_prompt, int max_tokens,
                                             double temperature, cJSON *initial_messages,
                                             cJSON **updated_messages, agent_result_t *out)
{
   if (updated_messages)
      *updated_messages = NULL;
   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);
   snprintf(out->model, MAX_MODEL_LEN, "%s", agent->model);
   snprintf(out->served_model, MAX_MODEL_LEN, "%s", agent->model);
   agent_t native_provider_agent;

   /* Dispatch to tmux-CLI backend if configured. The tmux session driver
    * (cli_session) runs on the client over the reverse channel when the turn's
    * workspace is detached, so a thin-client `claude` agent runs the standard
    * `claude` CLI over tmux on the client — no `claude -p` involved. */
   if (strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0)
      return agent_execute_cli_session(agent, network, system_prompt, user_prompt, max_tokens,
                                       temperature, out);

   /* Dispatch to provider-CLI backend. Some legacy provider-CLI configs now
    * bridge into Aimee's native HTTP provider loop instead of spawning a CLI. */
   if (strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 ||
       strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) == 0)
   {
      const provider_cli_adapter_t *adapter = provider_cli_adapter_get(agent->cli_kind);
      if (adapter && adapter->native_provider && adapter->native_provider[0])
      {
         char err[512];
         if (provider_cli_adapter_prepare_native_agent(adapter, agent, &native_provider_agent, err,
                                                       sizeof(err)) != 0)
         {
            snprintf(out->error, sizeof(out->error), "%s", err[0] ? err : "native adapter error");
            return -1;
         }
         agent = &native_provider_agent;
         goto native_provider_http;
      }
      if (adapter)
         return provider_cli_adapter_execute(adapter, agent, run_cmd_get_cwd(), system_prompt,
                                             user_prompt, out);
      snprintf(out->error, sizeof(out->error),
               "provider-cli: unknown cli_kind '%s' (expected: codex, claude, mistral, "
               "mistral-plan, vibe-plan)",
               agent->cli_kind);
      return -1;
   }

native_provider_http:
   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }

   struct timespec loop_start;
   clock_gettime(CLOCK_MONOTONIC, &loop_start);
   int total_timeout_ms = agent->timeout_ms * 4;

   /* Ephemeral SSH setup */
   char ephemeral_key[MAX_PATH_LEN] = {0};
   char session_id[128] = {0};
   int has_ephemeral_ssh = 0;

   /* Start tunnels if configured */
   int has_tunnels = 0;
   if (network && network->tunnel_mgr && network->tunnel_mgr->tunnel_count > 0)
   {
      agent_tunnel_mgr_init(network->tunnel_mgr);
      if (agent_tunnel_start_all(network->tunnel_mgr) == 0)
         has_tunnels = 1;
   }

   /* Build effective network config with ephemeral key if available */
   agent_network_t eff_network;
   if (network && network->ssh_entry[0])
   {
      memcpy(&eff_network, network, sizeof(eff_network));
      if (agent_ssh_setup(network, ephemeral_key, sizeof(ephemeral_key), session_id,
                          sizeof(session_id)) == 0)
      {
         has_ephemeral_ssh = 1;
         /* Update the effective network's ssh_entry to use the ephemeral key */
         char new_entry[512];
         snprintf(new_entry, sizeof(new_entry), "%s -i %s", network->ssh_entry, ephemeral_key);
         snprintf(eff_network.ssh_entry, sizeof(eff_network.ssh_entry), "%s", new_entry);
         snprintf(eff_network.ssh_key, sizeof(eff_network.ssh_key), "%s", ephemeral_key);
      }
   }
   else
   {
      memset(&eff_network, 0, sizeof(eff_network));
   }

   /* Link tunnel manager to effective network */
   if (has_tunnels)
      eff_network.tunnel_mgr = network->tunnel_mgr;

   /* Build context-rich system prompt */
   int current_code_only = agent_tools_role_current_code_only(role);
   char *assembled_sys = agent_build_exec_context_ex(
       agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL), system_prompt,
       current_code_only);
   const char *sys = assembled_sys ? assembled_sys : system_prompt;
   if (!sys || !sys[0])
      sys = current_code_only
                ? "# Instructions\n"
                  "- Use only current-checkout evidence: read_file, list_files, grep, git tools, "
                  "and shell commands against this workspace.\n"
                  "- Do not use Aimee memory, docs, index, search, notes, learning, or remote MCP "
                  "tools.\n"
                  "- When you have completed the task, respond with a final summary.\n"
                : default_exec_instructions;

   /* Resolve auth */
   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "auth resolution failed");
      free(assembled_sys);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      return -1;
   }
   char extra_headers[512];
   agent_build_extra_headers(agent, extra_headers, sizeof(extra_headers));

   /* Mutable copy for model fallback */
   agent_t fb_agent;
   memcpy(&fb_agent, agent, sizeof(fb_agent));

   /* Build URL via the provider driver so host-root and trailing-slash
    * OpenAI-compatible endpoints normalize the same way everywhere. */
   char url[MAX_ENDPOINT_LEN + 64];
   int chatgpt = is_chatgpt_provider(agent);
   int anthropic = is_anthropic_provider(agent);
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   if (delegate_build_url(driver, agent, url, sizeof(url)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "failed to build request URL");
      return -1;
   }

   /* Build tools JSON (reused each turn, format depends on provider) */
   cJSON *tools = chatgpt     ? build_tools_array_responses()
                  : anthropic ? build_tools_array_anthropic()
                              : build_tools_array(); /* OpenAI format */
   agent_tools_filter_for_role(tools, role);
   int inject_respond_tool = agent_should_inject_respond_tool(agent, role);
   if (inject_respond_tool)
      agent_tools_append_delegate_respond_tool(tools);
   agent_tools_set_dispatch_role(role);
   /* Provider-specific schema sanitization. Only fires for local llama.cpp-style
    * providers (including Qwen served behind an OpenAI-compatible endpoint)
    * and ollama (others pass through). Operates on the OpenAI-format
    * builder output — chatgpt's Responses API and the anthropic-specific
    * builder already shape the schema for their providers. */
   if (!chatgpt && !anthropic)
      agent_tools_sanitize_for_agent(tools, agent);

   /* Build conversation history. Primary sessions pass structured provider
    * history here; delegate runs start empty and remain single-task. */
   cJSON *messages = initial_messages ? cJSON_Duplicate(initial_messages, 1) : cJSON_CreateArray();
   int has_prior_messages = messages && cJSON_GetArraySize(messages) > 0;

   /* For OpenAI, system prompt goes in messages array.
    * For Anthropic and ChatGPT it goes in the request body, handled by the
    * respective request builder. */
   if (!has_prior_messages && !chatgpt && !anthropic)
   {
      cJSON *sys_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(sys_msg, "role", "system");
      cJSON_AddStringToObject(sys_msg, "content", sys);
      cJSON_AddItemToArray(messages, sys_msg);
   }

   cJSON *user_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(user_msg, "role", "user");
   cJSON_AddStringToObject(user_msg, "content", user_prompt);
   cJSON_AddItemToArray(messages, user_msg);

   int turn = 0;
   int total_calls = 0;
   /* §3 fold-freeze: per-run boundary state, persisted across turns so the folded
    * prefix stays byte-identical (warm provider cache). Honored only when
    * fold_freeze_enabled; ignored otherwise. */
   fold_freeze_t agent_fold_freeze;
   memset(&agent_fold_freeze, 0, sizeof(agent_fold_freeze));
   /* Context economizer (delegate seam): per-run reducer state persisted across
    * turns so the §3 fold-freeze boundary stays byte-identical (warm provider
    * cache). Honored only when reduce.delegate_seam is enabled. */
   reduce_state_t agent_reduce_state;
   memset(&agent_reduce_state, 0, sizeof(agent_reduce_state));
   /* §4 fold recall: per-run page table of folded-away coordinates, for re-touch
    * hints across turns. Honored only when fold_recall_enabled. */
   fold_recall_index_t agent_fold_recall;
   fold_recall_index_init(&agent_fold_recall);
   int api_call_count = 0; /* cumulative provider API calls; published via heartbeat */
   int final_instruction_added = 0;
   int final_tool_retry_count = 0;
   int degenerate_retry_count = 0;
   int tok = agent_request_max_tokens(agent, max_tokens);
   int max_t = agent_resolve_max_turns(agent, role);
   int initial_max_t = max_t;
   int final_after_turns = delegate_final_after_turns_for_role(role);

   /* Stuck detection state */
   char last_tool_sig[256] = {0};
   char last_tool_name[64] = {0};
   char last_tool_result[512] = {0};
   int repeat_count = 0;
   int total_repeat_triggers =
       0;   /* circuit breaker: aborts after LIVENESS_REPEAT_ABORT_THRESHOLD */
   (void)0; /* transient retries handled by http_retry_post */

   /* Adaptive context refresh: more frequent for complex tasks (many tool calls),
    * less frequent for simple ones. Start at 5, decrease as calls accumulate. */
   int refresh_interval = 5;

   /* Consecutive tool-error counter: increments on each tool-error result,
    * resets to 0 on any successful tool call. Used by mw_stall_detect. */
   int consecutive_errors = 0;

   /* Middleware pipeline: composable per-turn hooks for loop control.
    * Built from per-agent config with sensible defaults.
    * Primary sessions (role == NULL) pass max_turns=0 so the turn-limit and
    * final-response-nudge middleware are not registered — those are
    * delegate-specific.  The while-loop condition (turn < max_t) remains the
    * actual safety net. */
   /* Post-compaction recovery accounting (see headers/rounds_to_resume.h).
    * Inactive until a compaction actually happens, so a session that never
    * compacts never touches it. */
   rtr_tracker_t rtr;
   memset(&rtr, 0, sizeof(rtr));

   int mw_max_turns = role ? max_t : 0;
   mw_pipeline_t mw_pipeline;
   mw_pipeline_cfgs_t mw_cfgs;
   mw_pipeline_build(&mw_pipeline, &mw_cfgs, &agent->middleware, mw_max_turns, agent->model);

   while (turn < max_t)
   {
      char cancel_reason[128];
      if (agent_durable_cancelled(cancel_reason, sizeof(cancel_reason)))
      {
         snprintf(out->error, sizeof(out->error), "%s before turn %d", cancel_reason, turn + 1);
         break;
      }

      /* Check total timeout */
      struct timespec now_ts;
      clock_gettime(CLOCK_MONOTONIC, &now_ts);
      int elapsed_ms = (int)((now_ts.tv_sec - loop_start.tv_sec) * 1000 +
                             (now_ts.tv_nsec - loop_start.tv_nsec) / 1000000);
      if (elapsed_ms > total_timeout_ms)
      {
         snprintf(out->error, sizeof(out->error), "total timeout exceeded (%dms)", elapsed_ms);
         break;
      }

      /* Run middleware pipeline before each turn */
      {
         mw_loop_ctx_t mw_ctx;
         mw_ctx.turn = turn;
         mw_ctx.max_turns = max_t;
         mw_ctx.prompt_tokens = out->prompt_tokens;
         mw_ctx.completion_tokens = out->completion_tokens;
         mw_ctx.context_window = agent_effective_context_window(agent);
         mw_ctx.tool_calls = total_calls;
         mw_ctx.consecutive_errors = consecutive_errors;

         mw_pipeline_cfgs_set_max_turns(&mw_cfgs, role ? max_t : 0);
         mw_result_t mw_res = mw_pipeline_run(&mw_pipeline, &mw_ctx);

         if (mw_res.action == MW_STOP)
         {
            if (mw_res.reason[0])
               snprintf(out->error, sizeof(out->error), "middleware: %s", mw_res.reason);
            break;
         }

         if (mw_res.action == MW_COMPACT)
         {
            aimee_log(LOG_INFO, "agent", "middleware: compact requested: %s", mw_res.reason);
            session_compact_result_t sc_result;
            session_compact(messages, NULL, &sc_result);
            if (sc_result.compacted)
            {
               aimee_log(LOG_INFO, "agent",
                         "session compacted: %d→%d messages (%d removed, %d repairs)",
                         sc_result.messages_before, sc_result.messages_after,
                         sc_result.messages_removed, sc_result.repairs);
               /* Arm recovery accounting for the turns after this boundary. The
                * pre-boundary lookups are only knowable here — session_compact
                * has just destroyed the messages they came from. */
               rtr_begin(&rtr, &sc_result);
            }
            else
            {
               /* Fallback: basic repair + consecutive merge */
               message_history_repair(messages);
               messages_compact_consecutive(messages);
            }
            /* fall through: continue with the compacted history */
         }

         if (mw_res.action == MW_INJECT && mw_res.message[0])
         {
            cJSON *inject_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(inject_msg, "role", "user");
            cJSON_AddStringToObject(inject_msg, "content", mw_res.message);
            cJSON_AddItemToArray(messages, inject_msg);
         }
      }

      /* Adaptive context refresh: adjust interval based on task complexity */
      if (turn > 0 && (turn % refresh_interval) == 0)
      {
         int do_refresh = 1;
         if (anthropic && anthropic_context_refresh_deferred(driver, &fb_agent, messages, sys))
            do_refresh = 0;
         if (do_refresh)
         {
            char *refreshed = agent_build_exec_context_ex(
                agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL), system_prompt,
                current_code_only);
            if (refreshed)
            {
               free(assembled_sys);
               assembled_sys = refreshed;
               sys = assembled_sys;

               /* Update system message in conversation for OpenAI providers */
               if (!chatgpt && !anthropic)
               {
                  cJSON *first = cJSON_GetArrayItem(messages, 0);
                  if (first)
                     cJSON_ReplaceItemInObject(first, "content", cJSON_CreateString(sys));
               }
            }
         }
      }

      /* Repair inconsistent history (orphaned tool calls/results) then compact */
      message_history_repair(messages);
      messages_compact_consecutive(messages);

      maybe_compact_before_request(&fb_agent, messages, (chatgpt || anthropic) ? sys : NULL);

      /* Primary sessions (role == NULL) never close the tool budget — the user
       * can always send another message.  Pass max_turns=0 so HARD never fires. */
      liveness_final_response_mode_t final_mode =
          liveness_final_response_mode(turn, role ? max_t : 0, total_calls, final_after_turns);
      int final_instruction_turn = final_mode != LIVENESS_FINAL_RESPONSE_NONE;
      int final_text_only_turn = !liveness_final_response_allows_tools(final_mode);
      if (final_instruction_turn && !final_instruction_added)
      {
         agent_session_append_final_instruction(messages);
         final_instruction_added = 1;
      }
      cJSON *active_tools = final_text_only_turn ? NULL : tools;

      /* Context economizer (delegate seam): record a baseline + foldable-opportunity
       * ledger row, and — when reduce.history_fold is on — ACTUALLY fold the prefix
       * (provider-agnostic rolling skeleton + Coordinate Closet). The reduced view,
       * when produced, replaces `messages` for the request build of every provider
       * branch below and is freed after the request is serialized. Cheap
       * (mtime-cached) config load keeps this dark by default; default-off is
       * byte-identical to the prior behavior. */
      reduce_result_t agent_reduce_result;
      memset(&agent_reduce_result, 0, sizeof(agent_reduce_result));
      int reduce_active = 0; /* 1 once a real reduction replaced the message array */
      {
         config_t ecfg;
         if (config_load(&ecfg) == 0 && ecfg.reduce_delegate_seam &&
             (ecfg.reduce_measure_enabled || ecfg.reduce_history_fold || ecfg.reduce_compress))
         {
            reduce_config_t rcfg;
            memset(&rcfg, 0, sizeof(rcfg));
            rcfg.delegate_seam = 1;
            /* The synthetic fold turns are {role,content:string} — valid input for
             * anthropic / openai builders, but the chatgpt Responses
             * builder passes the array through unconverted and its API expects
             * top-level typed items, so feeding it folded turns is unverified.
             * Keep the Responses path measure-only here; fold it in a later slice
             * once verified live. */
            /* P3 master-kill: economizer.enabled=false forces the MUTATING levers off but
             * leaves the block reachable, so measure_only shadow accounting keeps running
             * (reports zero reductions — proof the kill works, per the two-tier contract). */
            int econ_master = econ_reduction_master_on(&ecfg); /* tier != off */
            int aggressive = econ_tier(&ecfg) == ECON_TIER_AGGRESSIVE;
            /* history_fold is the SAFE-tier lever: recall-restorable (non-destructive) and
             * freeze-guarded, so it is cache-favorable even on Anthropic (frozen on first
             * send). Runs on safe+aggressive. (!chatgpt: the Responses builder can't take
             * folded turns yet — a separate constraint, not a tier decision.) */
            rcfg.history_fold = econ_master && ecfg.reduce_history_fold && !chatgpt;
            /* Compress LOSSILY shrinks tool-result bodies in place -> the AGGRESSIVE tier
             * only. Shape (role/type, ids, typed items) is preserved, so it is valid for
             * all builders including chatgpt/Responses. */
            rcfg.compress = aggressive && ecfg.reduce_compress;
            rcfg.measure_only =
                !(rcfg.history_fold || rcfg.compress); /* a lever on -> mutate; else shadow */
            /* Tier model: fold is ALWAYS freeze-guarded (cache-favorability gated). This is
             * what makes fold cache-safe on Anthropic -- the folded prefix is frozen on
             * first send and only re-folded when the savings beat the cache-write cost over
             * the horizon, so a cache miss never costs more than it saves. Not togglable. */
            rcfg.freeze_guard_enabled = 1;
            rcfg.freeze_guard_horizon =
                ecfg.reduce_freeze_guard_horizon > 0 ? ecfg.reduce_freeze_guard_horizon : 1;
            rcfg.fold.retained_msgs = ecfg.fold_retained_msgs;
            rcfg.fold.min_fold_msgs = ecfg.fold_min_fold_msgs;
            rcfg.fold.reasoning_excerpt_bytes = ecfg.fold_excerpt_bytes;
            rcfg.fold.compact_head_bytes = ecfg.compact_head_bytes; /* compact.* drives the */
            rcfg.fold.compact_tail_bytes = ecfg.compact_tail_bytes; /* shared shrink core    */
            rcfg.fold.register_enabled = ecfg.fold_register_enabled;
            rcfg.fold.closet.enabled = ecfg.coord_closet_enabled;
            rcfg.fold.closet.budget_bytes = ecfg.coord_closet_budget_bytes;
            rcfg.fold.closet.max_ratio_pct = ecfg.coord_closet_max_ratio_pct;
            rcfg.fold.closet.denylist =
                ecfg.coord_closet_denylist[0] ? ecfg.coord_closet_denylist : NULL;
            /* Per-turn provenance: each loop turn is a distinct request at the single
             * delegate seam, so clear the cross-seam `reduced` flag while preserving
             * the across-turn freeze boundary in agent_reduce_state.freeze. */
            agent_reduce_state.reduced = 0;
            agent_reduce_state.turn = turn;
            /* session_id param unused by the transform; the ledger writer resolves
             * the session itself (a local `session_id[]` array shadows the fn here). */
            if (context_reduce(messages, sys, fb_agent.model, NULL, REDUCE_SEAM_DELEGATE, &rcfg,
                               &agent_reduce_state, &agent_reduce_result) == 0)
            {
               agent_record_reduce_ledger(&agent_reduce_result, fb_agent.model, agent->name, role);
               if (agent_reduce_result.mutated && agent_reduce_result.messages)
                  reduce_active = 1;
            }
            else
            {
               /* hard bypass: internal failure -> forward the original transcript */
               context_reduce_result_free(&agent_reduce_result);
               memset(&agent_reduce_result, 0, sizeof(agent_reduce_result));
            }
         }
      }
      /* When the economizer produced a reduced view, every provider branch builds
       * its request from it (and the Anthropic-only build_fold_view path is skipped
       * to avoid double-folding). */
      cJSON *eff_messages = reduce_active ? agent_reduce_result.messages : messages;

      /* Build request (use fb_agent which may have fallback model after turn 0) */
      cJSON *req;
      fold_result_t fold_view; /* §1 rolling fold; freed after req is serialized */
      memset(&fold_view, 0, sizeof(fold_view));
      if (chatgpt)
         req = agent_build_request_responses(&fb_agent, eff_messages, active_tools, sys);
      else if (anthropic)
      {
         /* Fold first so payload-rewrite tracking and the request both observe the
          * actually-sent (possibly folded) message array. The economizer's reduced
          * view takes precedence; otherwise fall back to the Anthropic build_fold_view. */
         cJSON *anth_msgs = eff_messages;
         if (!reduce_active)
         {
            build_fold_view(messages, &agent_fold_freeze, &agent_fold_recall, turn, &fold_view);
            if (fold_view.folded)
               anth_msgs = fold_view.messages;
         }
         track_anthropic_payload_rewrite(driver, &fb_agent, anth_msgs, sys);
         req = agent_build_request_anthropic(&fb_agent, anth_msgs, active_tools, sys, tok,
                                             temperature);
      }
      else
         req = agent_build_request_openai(&fb_agent, eff_messages, active_tools, tok, temperature);

      /* Universal-gateway P4: run aimee's own outbound call through the same request
       * pipeline the proxy ingresses use, so a config-enabled tool-policing policy
       * (gateway_prevent_subagents) applies to delegate calls. Mutates req in place;
       * a <0 return is a hard stage failure — abort rather than forward a half-altered
       * request (mirrors anthropic_http.c). Gated to delegates (role != NULL); the
       * primary shares the pipeline but is not policed here. */
      if (gateway_delegate_run_request_pipeline(
              req, gateway_delegate_tool_shape(anthropic, chatgpt), role != NULL) < 0)
      {
         snprintf(out->error, sizeof(out->error), "gateway request pipeline failed");
         cJSON_Delete(req);
         fold_result_free(&fold_view);
         context_reduce_result_free(&agent_reduce_result);
         break;
      }

      char *body = cJSON_PrintUnformatted(req);
      /* Order matters: req may hold a non-owning reference into fold_view.messages
       * OR agent_reduce_result.messages (agent_build_request_anthropic's
       * AddItemReference fallback), so req must be deleted before either is freed. */
      cJSON_Delete(req);
      fold_result_free(&fold_view);
      context_reduce_result_free(&agent_reduce_result);
      if (!body)
      {
         snprintf(out->error, sizeof(out->error), "failed to build request");
         break;
      }

      /* Log request trace */
      agent_trace_log(0, turn, "request", body, NULL, NULL, NULL, NULL);

      /* POST with automatic retry on transient errors. The per-call timeout is
       * capped by the remaining loop budget so a single model call can't blow
       * the total -- but once too little budget remains for a viable call,
       * issuing one anyway just yields a short-timeout read failure (HTTP -1)
       * that the provider-health tracker misreads as "provider unreachable" and
       * aborts the whole delegate (the provider is fine, we starved the call).
       * agent_loop_per_call_timeout_ms returns -1 in that case so we stop the
       * loop cleanly with a partial result instead. */
      char *response_body = NULL;
      int per_call =
          agent_loop_per_call_timeout_ms(agent->timeout_ms, total_timeout_ms, elapsed_ms);
      if (per_call < 0)
      {
         snprintf(out->error, sizeof(out->error), "tool loop budget exhausted (%dms of %dms used)",
                  elapsed_ms, total_timeout_ms);
         free(body);
         break;
      }

      config_t retry_cfg;
      config_load(&retry_cfg);
      int ra =
          retry_cfg.retry_max_attempts > 0 ? retry_cfg.retry_max_attempts : HTTP_RETRY_MAX_ATTEMPTS;
      int rb = retry_cfg.retry_base_ms > 0 ? retry_cfg.retry_base_ms : HTTP_RETRY_BASE_MS;
      int rm = retry_cfg.retry_max_ms > 0 ? retry_cfg.retry_max_ms : HTTP_RETRY_MAX_MS;

      {
         int dj = agent_get_durable_job_id();
         if (dj > 0)
            db1_agent_job_heartbeat_ext(dj, final_text_only_turn ? "final_response" : "model",
                                        api_call_count);
      }
      int http_status =
          http_retry_post_context(url, auth_header, body, &response_body, per_call, extra_headers,
                                  ra, rb, rm, agent->provider, fb_agent.model, session_id);
      api_call_count++;
      {
         int dj = agent_get_durable_job_id();
         if (dj > 0)
            db1_agent_job_heartbeat_ext(dj, "", api_call_count);
      }
      free(body);

      /* Model fallback on first turn: if 400, retry with fallback_model */
      if (http_status == 400 && turn == 0 && fb_agent.fallback_model[0])
      {
         free(response_body);
         response_body = NULL;
         snprintf(fb_agent.model, MAX_MODEL_LEN, "%s", fb_agent.fallback_model);
         fb_agent.fallback_model[0] = '\0';
         /* The fallback model is now the served model — record it for accounting. */
         snprintf(out->model, MAX_MODEL_LEN, "%s", fb_agent.model);
         snprintf(out->served_model, MAX_MODEL_LEN, "%s", fb_agent.model);

         cJSON *fb_req;
         fold_result_t fb_fold_view; /* §1 rolling fold; freed after fb_req serialized */
         memset(&fb_fold_view, 0, sizeof(fb_fold_view));
         if (chatgpt)
            fb_req = agent_build_request_responses(&fb_agent, messages, active_tools, sys);
         else if (anthropic)
         {
            build_fold_view(messages, &agent_fold_freeze, NULL, turn, &fb_fold_view);
            cJSON *fb_anth_msgs = fb_fold_view.folded ? fb_fold_view.messages : messages;
            track_anthropic_payload_rewrite(driver, &fb_agent, fb_anth_msgs, sys);
            fb_req = agent_build_request_anthropic(&fb_agent, fb_anth_msgs, active_tools, sys, tok,
                                                   temperature);
         }
         else
            fb_req =
                agent_build_request_openai(&fb_agent, messages, active_tools, tok, temperature);
         char *fb_body = cJSON_PrintUnformatted(fb_req);
         /* delete fb_req before freeing the fold view (req may reference it) */
         cJSON_Delete(fb_req);
         fold_result_free(&fb_fold_view);
         if (fb_body)
         {
            {
               int dj = agent_get_durable_job_id();
               if (dj > 0)
                  db1_agent_job_heartbeat_ext(dj, "model", api_call_count);
            }
            http_status = http_retry_post_context(url, auth_header, fb_body, &response_body,
                                                  per_call, extra_headers, ra, rb, rm,
                                                  fb_agent.provider, fb_agent.model, session_id);
            api_call_count++;
            {
               int dj = agent_get_durable_job_id();
               if (dj > 0)
                  db1_agent_job_heartbeat_ext(dj, "", api_call_count);
            }
            free(fb_body);
         }
      }

      /* Update provider health cache */
      provider_health_update(agent->provider, http_status);

      /* --- Context overflow: truncate old messages and retry --- */
      if (http_status == 400 && response_body &&
          strstr(response_body, "exceeds the available context"))
      {
         free(response_body);
         response_body = NULL;

         /* Remove oldest non-system, non-first-user messages to free space.
          * Keep: [0] system, [1] first user message, then trim from index 2. */
         int msg_count = cJSON_GetArraySize(messages);
         int removed = 0;
         while (msg_count > 4 && removed < (msg_count / 3))
         {
            cJSON_DeleteItemFromArray(messages, 2);
            removed++;
            msg_count = cJSON_GetArraySize(messages);
         }
         if (removed > 0)
         {
            aimee_log(LOG_INFO, "agent", "context overflow, truncated %d messages, retrying...",
                      removed);
            continue; /* retry same turn with shorter context */
         }
         /* Nothing left to truncate */
         snprintf(out->error, sizeof(out->error),
                  "provider '%s' (HTTP 400): context overflow, cannot truncate further",
                  agent->name);
         break;
      }

      if (http_status < 0 || !response_body)
      {
         provider_err_class_t cls = provider_classify_error(http_status);
         snprintf(out->error, sizeof(out->error),
                  "provider '%s' %s Provider-backed delegation is unavailable; "
                  "server-routed local commands can still run. Review "
                  "~/.config/aimee/agents.json for diagnostics.",
                  agent->name, provider_error_message(cls));
         free(response_body);
         break;
      }

      if (http_status != 200)
      {
         provider_err_class_t cls = provider_classify_error(http_status);
         char snippet[128] = {0};
         if (response_body)
            snprintf(snippet, sizeof(snippet), "%.120s", response_body);
         snprintf(out->error, sizeof(out->error), "provider '%s' (HTTP %d): %s %s", agent->name,
                  http_status, provider_error_message(cls), snippet);
         free(response_body);
         break;
      }

      /* (transient retries handled by http_retry_post) */

      /* Parse response */
      parsed_response_t parsed;
      /* XML tool-call rescue gate (models without native function-calling embed
       * <tool_call> blocks in text). Computed up front because the IR parser now
       * OWNS the rescue: rescue_mode<0 disables it, 0 rescues dialect calls, 1 also
       * rescues bare JSON. ir_primary/n_rescued carry the parse outcome down to the
       * rescue-policy step below. */
      int rescue_enabled = !fb_agent.ablation.configured || fb_agent.ablation.rescue;
      int allow_json_rescue = rescue_enabled ? agent_allows_json_content_rescue(&fb_agent) : 0;
      int rescue_mode = rescue_enabled ? allow_json_rescue : -1;
      int ir_primary = 0;
      int n_rescued = 0;
      if (chatgpt)
      {
         /* IR is the SOLE parser for the responses/SSE (codex) wire -- the legacy SSE
          * parser is gone (shadow-proven at parity on live .254 traffic). It owns the
          * XML rescue (via rescue_mode); ir_primary / n_rescued carry the outcome to
          * the rescue-policy step below. A parse failure yields an empty response. */
         ir_primary =
             agent_ir_parse_responses(response_body, rescue_mode, &n_rescued, &parsed) == 0;
         if (!ir_primary)
            memset(&parsed, 0, sizeof(parsed));
      }
      else
      {
         cJSON *root = cJSON_Parse(response_body);
         if (root)
         {
            /* IR is the SOLE response parser for the JSON wires -- the legacy
             * translators are gone (shadow-proven at parity, 0 fallbacks on live
             * traffic). It also owns the XML rescue (via rescue_mode); ir_primary /
             * n_rescued carry the outcome to the rescue-policy step below. A parse
             * failure yields an empty response, the same as an unparseable body. */
            ir_primary = agent_ir_parse_json_response(root, anthropic, rescue_mode, &n_rescued,
                                                      &parsed) == 0;
            if (!ir_primary)
               memset(&parsed, 0, sizeof(parsed));
            cJSON_Delete(root);
         }
         else
         {
            memset(&parsed, 0, sizeof(parsed));
         }
      }
      free(response_body);

      /* Universal-gateway P4 (response side): police a denied tool_use the model
       * emitted anyway. Shape-agnostic — operates on the normalized parsed_response_t,
       * so it covers every provider. Config-gated internally and gated
       * to delegate calls here: the primary spawns delegates via the very Task/Subagent
       * tool this would drop, so policing the primary's response would neuter aimee's
       * own delegation. Drop count discarded, as in the ingress (anthropic_http.c). */
      if (role != NULL)
         gateway_policy_police_parsed_response(&parsed);

      /* Log response trace */
      agent_trace_log(0, turn, "response", parsed.content ? parsed.content : "(tool_calls)", NULL,
                      NULL, NULL, NULL);

      /* Accumulate tokens */
      out->prompt_tokens += parsed.prompt_tokens;
      out->completion_tokens += parsed.completion_tokens;
      out->cache_write_tokens += parsed.cache_write_tokens;
      out->cache_read_tokens += parsed.cache_read_tokens;

      /* XML tool-call rescue: models without native function-calling embed
       * <tool_call>...</tool_call> blocks in their text. When the IR was primary it
       * ALREADY rescued -- the parser owns it now (aimee_ir_rescue_tool_calls) -- so
       * parsed already carries any recovered calls and n_rescued reports the count.
       * The legacy path (chatgpt wire + any IR fallback) keeps its own post-parse
       * rescue in the else branch. */
      if (ir_primary)
      {
         /* A FINAL text-only turn that smuggled a tool call (rescued from prose) is a
          * policy violation: it should have answered, not called a tool. Detected via
          * n_rescued instead of re-scanning the text. */
         if (final_text_only_turn && n_rescued > 0)
         {
            const char *attempted_tool = (parsed.call_count > 0 && parsed.calls[0].name[0])
                                             ? parsed.calls[0].name
                                             : "XML tool_call block";
            /* If the smuggled call was actually the respond tool, it IS a final
             * answer -- strip it and finish successfully. */
            if (inject_respond_tool && agent_tools_strip_delegate_respond(&parsed) == 1 &&
                parsed.content && !liveness_is_degenerate_response(parsed.content))
            {
               agent_session_append_final_message(messages, parsed.content);
               out->response = parsed.content;
               parsed.content = NULL;
               out->rescue_recoveries++;
               out->success = 1;
               agent_free_parsed_response(&parsed);
               break;
            }
            if (agent_session_retry_final_tool_violation(messages, attempted_tool, &turn, &max_t,
                                                         initial_max_t, &final_tool_retry_count,
                                                         out->error, sizeof(out->error)))
            {
               agent_free_parsed_response(&parsed);
               continue;
            }
            finish_final_tool_violation(out, agent, attempted_tool, parsed.content, last_tool_name,
                                        last_tool_result, total_calls);
            agent_free_parsed_response(&parsed);
            break;
         }
         if (n_rescued > 0)
            out->rescue_recoveries++;
      }
      else if (rescue_enabled && !parsed.is_tool_call && parsed.content &&
               delegate_rescue_has_tool_calls_with_json(parsed.content, allow_json_rescue))
      {
         if (final_text_only_turn)
         {
            const char *attempted_tool = "XML tool_call block";
            parsed_response_t xml_final;
            memset(&xml_final, 0, sizeof(xml_final));
            if (delegate_rescue_parse_tool_calls(parsed.content, &xml_final, allow_json_rescue) >
                    0 &&
                xml_final.call_count > 0 && xml_final.calls[0].name[0])
               attempted_tool = xml_final.calls[0].name;

            if (inject_respond_tool && agent_tools_strip_delegate_respond(&xml_final) == 1 &&
                xml_final.content && !liveness_is_degenerate_response(xml_final.content))
            {
               agent_session_append_final_message(messages, xml_final.content);
               out->response = xml_final.content;
               xml_final.content = NULL;
               out->rescue_recoveries++;
               out->success = 1;
               agent_free_parsed_response(&xml_final);
               agent_free_parsed_response(&parsed);
               break;
            }

            if (agent_session_retry_final_tool_violation(messages, attempted_tool, &turn, &max_t,
                                                         initial_max_t, &final_tool_retry_count,
                                                         out->error, sizeof(out->error)))
            {
               agent_free_parsed_response(&xml_final);
               agent_free_parsed_response(&parsed);
               continue;
            }
            finish_final_tool_violation(out, agent, attempted_tool, xml_final.content,
                                        last_tool_name, last_tool_result, total_calls);
            agent_free_parsed_response(&xml_final);
            agent_free_parsed_response(&parsed);
            break;
         }

         parsed_response_t xml_parsed;
         memset(&xml_parsed, 0, sizeof(xml_parsed));
         if (delegate_rescue_parse_tool_calls(parsed.content, &xml_parsed, allow_json_rescue) > 0)
         {
            free(parsed.content);
            parsed.content = xml_parsed.content;
            parsed.is_tool_call = 1;
            parsed.call_count = xml_parsed.call_count;
            memcpy(parsed.calls, xml_parsed.calls,
                   (size_t)xml_parsed.call_count * sizeof(xml_parsed.calls[0]));
            /* Original assistant_message has no tool_calls; clear it so the
             * conversation-history append doesn't record a malformed turn. */
            if (parsed.assistant_message)
            {
               cJSON_Delete(parsed.assistant_message);
               parsed.assistant_message = NULL;
            }
            if (!chatgpt && !anthropic)
               parsed.assistant_message = agent_build_openai_assistant_message_from_calls(&parsed);
            out->rescue_recoveries++;
         }
      }

      int respond_strip = inject_respond_tool ? agent_tools_strip_delegate_respond(&parsed) : 0;
      if (respond_strip == 2 && !chatgpt && !anthropic)
      {
         if (parsed.assistant_message)
            cJSON_Delete(parsed.assistant_message);
         parsed.assistant_message = agent_build_openai_assistant_message_from_calls(&parsed);
      }

      if (final_text_only_turn && parsed.is_tool_call)
      {
         const char *attempted_tool = (parsed.call_count > 0 && parsed.calls[0].name[0])
                                          ? parsed.calls[0].name
                                          : "tool call";
         if (agent_session_retry_final_tool_violation(messages, attempted_tool, &turn, &max_t,
                                                      initial_max_t, &final_tool_retry_count,
                                                      out->error, sizeof(out->error)))
         {
            agent_free_parsed_response(&parsed);
            continue;
         }
         finish_final_tool_violation(out, agent, attempted_tool, parsed.content, last_tool_name,
                                     last_tool_result, total_calls);
         agent_free_parsed_response(&parsed);
         break;
      }

      if (!parsed.is_tool_call)
      {
         /* Final text response */
         if (parsed.content)
         {
            if (liveness_is_degenerate_response(parsed.content))
            {
               if (total_calls == 0 && agent_session_retry_degenerate_response(
                                           messages, &turn, &degenerate_retry_count))
               {
                  agent_free_parsed_response(&parsed);
                  continue;
               }
               /* Empty or punctuation-only response — replace with explicit failure diagnostic. */
               char diag[512];
               liveness_format_empty_diagnostic(agent->name, diag, sizeof(diag));
               free(parsed.content);
               parsed.content = NULL;
               out->response = safe_strdup(diag);
               out->success = 0;
               snprintf(out->error, sizeof(out->error),
                        "delegate returned empty or degenerate response");
            }
            else
            {
               agent_session_append_final_message(messages, parsed.content);
               out->response = parsed.content;
               parsed.content = NULL; /* prevent agent_free_parsed_response from freeing it */
               out->success = 1;
            }
         }
         else
         {
            if (total_calls > 0 &&
                agent_session_retry_final_tool_violation(
                    messages, "an empty final response", &turn, &max_t, initial_max_t,
                    &final_tool_retry_count, out->error, sizeof(out->error)))
            {
               agent_free_parsed_response(&parsed);
               continue;
            }
            out->success = 0;
            snprintf(out->error, sizeof(out->error), "no content in final response");
         }
         agent_free_parsed_response(&parsed);
         break;
      }

      /* Execute tool calls */
      {
         char cancel_reason[128];
         if (agent_durable_cancelled(cancel_reason, sizeof(cancel_reason)))
         {
            snprintf(out->error, sizeof(out->error), "%s before tool execution at turn %d",
                     cancel_reason, turn + 1);
            agent_free_parsed_response(&parsed);
            break;
         }
      }

      if (anthropic && parsed.assistant_message)
      {
         /* Anthropic: assistant_message is the content array itself */
         cJSON *asst = cJSON_CreateObject();
         cJSON_AddStringToObject(asst, "role", "assistant");
         cJSON_AddItemToObject(asst, "content", cJSON_Duplicate(parsed.assistant_message, 1));
         cJSON_AddItemToArray(messages, asst);
      }
      else if (!chatgpt && parsed.assistant_message)
      {
         /* OpenAI: append the assistant message (with tool_calls) to conversation */
         cJSON *asst = cJSON_CreateObject();
         cJSON_AddStringToObject(asst, "role", "assistant");

         cJSON *tc_arr = cJSON_GetObjectItem(parsed.assistant_message, "tool_calls");
         if (tc_arr)
            cJSON_AddItemToObject(asst, "tool_calls", cJSON_Duplicate(tc_arr, 1));

         cJSON *asst_content = cJSON_GetObjectItem(parsed.assistant_message, "content");
         if (asst_content && !cJSON_IsNull(asst_content))
            cJSON_AddItemToObject(asst, "content", cJSON_Duplicate(asst_content, 1));
         else
            cJSON_AddNullToObject(asst, "content");

         cJSON_AddItemToArray(messages, asst);
      }
      else if (chatgpt && parsed.assistant_message)
      {
         /* Responses API: append only the function_call items we will actually
          * process (i.e. up to parsed.call_count). Appending calls beyond that
          * would cause the API to expect function_call_output for IDs we never
          * return, resulting in HTTP 400. */
         for (int i = 0; i < parsed.call_count; i++)
         {
            int n = cJSON_GetArraySize(parsed.assistant_message);
            for (int j = 0; j < n; j++)
            {
               cJSON *item = cJSON_GetArrayItem(parsed.assistant_message, j);
               cJSON *itype = cJSON_GetObjectItem(item, "type");
               cJSON *cid = cJSON_GetObjectItem(item, "call_id");
               if (itype && cJSON_IsString(itype) &&
                   strcmp(itype->valuestring, "function_call") == 0 && cid && cJSON_IsString(cid) &&
                   strcmp(cid->valuestring, parsed.calls[i].id) == 0)
               {
                  cJSON_AddItemToArray(messages, cJSON_Duplicate(item, 1));
                  break;
               }
            }
         }
      }

      /* For Anthropic, collect all tool_result blocks into one user message */
      cJSON *anth_results = anthropic ? cJSON_CreateArray() : NULL;

      /* Notify auto-snapshot context of the current turn so file writes can be grouped */
      agent_tools_begin_turn(turn);

      /* Rounds-to-resume accounting: judge what the model ASKED for, before the
       * tools run — a re-derivation is a re-derivation whether or not it
       * succeeds. Observation only; nothing here steers the loop. */
      if (rtr.active)
      {
         const char *rtr_names[AGENT_MAX_TOOL_CALLS];
         const char *rtr_args[AGENT_MAX_TOOL_CALLS];
         int rtr_n =
             parsed.call_count < AGENT_MAX_TOOL_CALLS ? parsed.call_count : AGENT_MAX_TOOL_CALLS;
         for (int i = 0; i < rtr_n; i++)
         {
            rtr_names[i] = parsed.calls[i].name;
            rtr_args[i] = parsed.calls[i].arguments;
         }
         if (rtr_observe_turn(&rtr, rtr_names, rtr_args, rtr_n))
            aimee_log(LOG_INFO, "agent",
                      "rounds_to_resume=%d rederived_calls=%d basis_sigs=%d basis_dropped=%d",
                      rtr.rounds_to_resume, rtr.rederived_calls, rtr.sig_count, rtr.sigs_dropped);
      }

      for (int i = 0; i < parsed.call_count; i++)
      {
         char cancel_reason[128];
         if (agent_durable_cancelled(cancel_reason, sizeof(cancel_reason)))
         {
            snprintf(out->error, sizeof(out->error), "%s before tool %d/%d at turn %d",
                     cancel_reason, i + 1, parsed.call_count, turn + 1);
            break;
         }

         /* Validate arguments against tool registry */
         char val_err[256] = {0};
         if (tool_validate(parsed.calls[i].name, parsed.calls[i].arguments, val_err,
                           sizeof(val_err)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: validation failed: %s", val_err);
            consecutive_errors++;
            /* Send error back to LLM so it can self-correct */
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s", parsed.calls[i].name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            free(err_result);
            total_calls++;
            continue;
         }

         /* Check policy */
         char policy_reason[256] = {0};
         const char *se = tool_side_effect(parsed.calls[i].name);
         if (policy_check_tool(parsed.calls[i].name, se, parsed.calls[i].arguments, policy_reason,
                               sizeof(policy_reason)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: blocked by policy: %s", policy_reason);
            consecutive_errors++;
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s", parsed.calls[i].name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            free(err_result);
            total_calls++;
            continue;
         }

         /* Check hard directives */
         char directive_reason[256] = {0};
         if (directive_check_tool(parsed.calls[i].name, parsed.calls[i].arguments, directive_reason,
                                  sizeof(directive_reason)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: blocked by directive: %s", directive_reason);
            consecutive_errors++;
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s", parsed.calls[i].name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            free(err_result);
            total_calls++;
            continue;
         }

         {
            int dj = agent_get_durable_job_id();
            if (dj > 0)
               db1_agent_job_heartbeat_ext(dj, parsed.calls[i].name, api_call_count);
         }
         char *result_str = dispatch_tool_call_ctx(parsed.calls[i].name, parsed.calls[i].arguments,
                                                   agent->timeout_ms);
         {
            int dj = agent_get_durable_job_id();
            if (dj > 0)
               db1_agent_job_heartbeat_ext(dj, "", api_call_count);
         }
         total_calls++;

         /* Track consecutive tool errors for mw_stall_detect */
         if (result_str && strncmp(result_str, "error", 5) == 0)
            consecutive_errors++;
         else
            consecutive_errors = 0;

         /* Log trace */
         agent_trace_log(0, turn, "tool_call", NULL, parsed.calls[i].name,
                         parsed.calls[i].arguments, result_str, NULL);
         snprintf(last_tool_name, sizeof(last_tool_name), "%s", parsed.calls[i].name);
         snprintf(last_tool_result, sizeof(last_tool_result), "%.500s",
                  result_str ? result_str : "");

         if (anthropic)
         {
            /* Anthropic: tool_result content block */
            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "type", "tool_result");
            cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
            cJSON_AddStringToObject(tr, "content", result_str ? result_str : "");
            cJSON_AddItemToArray(anth_results, tr);
         }
         else if (!chatgpt)
         {
            /* OpenAI format: role=tool, tool_call_id */
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
            cJSON_AddStringToObject(tool_msg, "content", result_str ? result_str : "");
            cJSON_AddItemToArray(messages, tool_msg);
         }
         else
         {
            /* Responses API: function_call_output */
            cJSON *out_item = cJSON_CreateObject();
            cJSON_AddStringToObject(out_item, "type", "function_call_output");
            cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
            cJSON_AddStringToObject(out_item, "output", result_str ? result_str : "");
            cJSON_AddItemToArray(messages, out_item);
         }

         free(result_str);
      }

      if (out->error[0] && strstr(out->error, "delegate cancelled"))
      {
         cJSON_Delete(anth_results);
         agent_free_parsed_response(&parsed);
         break;
      }

      /* Anthropic: append all tool results as a single user message */
      if (anth_results)
      {
         if (cJSON_GetArraySize(anth_results) > 0)
         {
            cJSON *user_tr = cJSON_CreateObject();
            cJSON_AddStringToObject(user_tr, "role", "user");
            cJSON_AddItemToObject(user_tr, "content", anth_results);
            cJSON_AddItemToArray(messages, user_tr);
         }
         else
         {
            cJSON_Delete(anth_results);
         }
      }

      /* Stuck detection: if the agent calls the same tool with the same args
       * LIVENESS_REPEAT_WARN_THRESHOLD times in a row, inject a warning.
       * Circuit breaker trips after LIVENESS_REPEAT_ABORT_THRESHOLD warnings. */
      if (parsed.call_count > 0)
      {
         char tool_sig[256];
         snprintf(tool_sig, sizeof(tool_sig), "%s:%.*s", parsed.calls[0].name, 200,
                  parsed.calls[0].arguments ? parsed.calls[0].arguments : "");
         if (strcmp(tool_sig, last_tool_sig) == 0)
         {
            repeat_count++;
            if (repeat_count >= LIVENESS_REPEAT_WARN_THRESHOLD)
            {
               total_repeat_triggers++;
               if (liveness_circuit_breaker_tripped(total_repeat_triggers))
               {
                  /* Hard circuit breaker: abort the loop */
                  snprintf(out->error, sizeof(out->error),
                           "circuit breaker tripped: repeated identical tool calls "
                           "(%d warning cycles, tool: %.100s)",
                           total_repeat_triggers, parsed.calls[0].name);
                  agent_free_parsed_response(&parsed);
                  break;
               }
               /* Inject a hint to the LLM that it appears stuck */
               cJSON *hint = cJSON_CreateObject();
               cJSON_AddStringToObject(hint, "role", "user");
               cJSON_AddStringToObject(hint, "content",
                                       "Warning: You appear to be repeating the same operation. "
                                       "Try a different approach or summarize your findings.");
               cJSON_AddItemToArray(messages, hint);
               repeat_count = 0;
               /* Also increase refresh frequency to give more context */
               if (refresh_interval > 2)
                  refresh_interval = 2;
            }
         }
         else
         {
            snprintf(last_tool_sig, sizeof(last_tool_sig), "%s", tool_sig);
            repeat_count = 0;
         }

         /* Adaptive refresh: after many tool calls, refresh more often */
         if (total_calls > 10 && refresh_interval > 3)
            refresh_interval = 3;
      }

      agent_free_parsed_response(&parsed);
      turn++;

      /* Update durable job cursor and heartbeat */
      int durable_job_id = agent_get_durable_job_id();
      if (durable_job_id > 0)
      {
         /* Cooperative cancellation: if the operator ran `aimee cancel job <id>`
          * (or another path flipped the row to 'cancelled'), honor it before
          * we update the row back to 'running' and start the next turn. */
         if (db1_agent_job_is_cancelled(durable_job_id))
         {
            snprintf(out->error, sizeof(out->error), "delegate cancelled (job #%d) at turn %d",
                     durable_job_id, turn);
            break;
         }
         db1_agent_job_update(durable_job_id, "running", turn, NULL);
         /* current_tool="" — between tool dispatches, the model is awaiting
          * the next response. The heartbeat_ext path keeps api_call_count
          * fresh on the row so external pollers can see forward progress. */
         db1_agent_job_heartbeat_ext(durable_job_id, "", api_call_count);
      }
   }

   fold_recall_index_free(&agent_fold_recall);

   /* If we exhausted turns without a final response, set a clear error */
   if (!out->success && !out->response && turn >= max_t)
   {
      snprintf(out->error, sizeof(out->error), "max turns exhausted (%d/%d) without final response",
               turn, max_t);
   }

   /* If the agent used tools but never produced final text, preserve enough
    * context for the parent/delegate status to be actionable. */
   if (!out->success && !out->response && total_calls > 0)
   {
      if (!out->error[0])
      {
         if (repeat_count >= 2)
            snprintf(out->error, sizeof(out->error),
                     "agent stuck: repeated same tool call %d times", repeat_count);
         else
            snprintf(out->error, sizeof(out->error),
                     "delegate stopped without a final response after tool use");
      }

      char partial[1024];
      snprintf(partial, sizeof(partial),
               "Delegate stopped before returning a final response.\n"
               "Reason: %s\n"
               "Turns: %d/%d\n"
               "Tool calls: %d\n"
               "Last tool: %s\n"
               "Last tool result excerpt: %.500s",
               out->error, turn, max_t, total_calls,
               last_tool_name[0] ? last_tool_name : "(unknown)",
               last_tool_result[0] ? last_tool_result : "(empty)");
      out->response = safe_strdup(partial);
      out->success = 0;
      out->abstained = 1;
      snprintf(out->abstain_reason, sizeof(out->abstain_reason),
               "partial result after tool use: %s", out->error);
   }

   /* Record timing */
   struct timespec end_ts;
   clock_gettime(CLOCK_MONOTONIC, &end_ts);
   out->latency_ms = (int)((end_ts.tv_sec - loop_start.tv_sec) * 1000 +
                           (end_ts.tv_nsec - loop_start.tv_nsec) / 1000000);
   out->turns = turn;
   out->tool_calls = total_calls;

   /* Confidence estimation and abstention */
   if (out->response)
      out->confidence = agent_estimate_confidence(out->response);
   else
      out->confidence = 0;

   if (out->confidence >= 0 && out->confidence < 50 && out->tool_calls > 0)
   {
      out->abstained = 1;
      snprintf(out->abstain_reason, sizeof(out->abstain_reason),
               "low confidence (%d) after %d tool calls", out->confidence, out->tool_calls);
   }

   if (out->success && updated_messages)
      *updated_messages = cJSON_Duplicate(messages, 1);

   /* Cleanup */
   agent_tools_set_dispatch_role(NULL);
   cJSON_Delete(tools);
   cJSON_Delete(messages);
   free(assembled_sys);

   /* Cleanup ephemeral SSH */
   if (has_ephemeral_ssh)
      agent_ssh_cleanup(network, ephemeral_key, session_id);

   /* Stop tunnels */
   if (has_tunnels && network && network->tunnel_mgr)
      agent_tunnel_stop_all(network->tunnel_mgr);

   /* Note: callers (agent_run, agent_run_with_tools) handle logging
    * with the correct role. Do not log here to avoid double-logging. */

   /* Write Prometheus metrics */
   agent_write_metrics();

   /* Write change manifest */
   {
      char run_id[64];
      snprintf(run_id, sizeof(run_id), "aimee-run-%d-%ld", (int)getpid(), (long)loop_start.tv_sec);
      agent_write_manifest(run_id, out, "");
   }

   /* Store execution outcome as feedback for future context */
   agent_store_feedback(out, "", user_prompt);

   return out->success ? 0 : -1;
}

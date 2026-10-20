#include <aimee/ir/session_event.h>
/* posix/agent_runtime.c: POSIX execution engine — agent loop, context assembly, and eval */
/* ================================================================
 * From: agent.c
 * ================================================================ */
/* posix/agent.c: POSIX implementation of agent_execute_with_tools. */
#include "aimee.h"
#include "agent.h"
#include "aimee_ir_shadow.h"
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h> /* anthropic_backend_parse / openai_backend_parse */
#include <aimee/ir/aimee_ir.h>               /* aimee_response_t + block accessors */
#include "tool_args_coerce.h"                /* tool_args_canonicalize_json */
#include "agent_exec.h"
#include "agent_protocol.h"
#include "server_session_log.h"
#include "server_session_capture.h"
#include <openssl/crypto.h>
#include "server_session_control.h"
#include "cli_session.h"
#include "db1_client/primary_sessions.h"
#include "agent_response_projection.h"
#include "agent_runtime_messages.h"
#include "ingress_preinject.h"
#include "request_context.h"
#include <aimee/tools/agent_tools.h>
#include "agent_tunnel.h"
#include <aimee/delegates/delegate_driver.h>
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/delegate_xml_fallback.h>
#include <aimee/gateway/gateway_policy.h>
#include "gw_stage_governance.h"
#include "http_retry.h"
#include "economizer.h"
#include "economizer_module_client.h"
#include "token_tracker.h"
#include "wire_fence.h"
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
#include "cJSON.h"
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static void delegate_response_release(aimee_response_t *response, char *text, char **arguments,
                                      int count)
{
   if (arguments)
   {
      for (int i = 0; i < count; i++)
         free(arguments[i]);
      free(arguments);
   }
   free(text);
   aimee_response_free(response);
}

static void delegate_capture_stop(agent_result_t *out, int *failed)
{
   *failed = 1;
   out->success = 0;
   snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
}

static int delegate_append_response(cJSON *messages, const aimee_response_t *response)
{
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   cJSON *item = aimee_ir_message_json(&message);
   if (!item || !cJSON_AddItemToArray(messages, item))
   {
      cJSON_Delete(item);
      return -1;
   }
   return 0;
}

static int delegate_append_tool_result(cJSON *messages, const char *id, const char *text,
                                       int failed)
{
   aimee_block_t block = {.type = AIMEE_BLK_TOOL_RESULT,
                          .tool_id = (char *)id,
                          .tool_is_error = failed,
                          .tool_result = cJSON_CreateString(text ? text : "")};
   aimee_message_t message = {.role = "tool", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   aimee_ir_classify_context(&request);
   cJSON *item = block.tool_result ? aimee_ir_message_json(&message) : NULL;
   cJSON_Delete(block.tool_result);
   if (!item || !cJSON_AddItemToArray(messages, item))
   {
      cJSON_Delete(item);
      return -1;
   }
   return 0;
}

static void finish_final_tool_violation(agent_result_t *out, const agent_t *agent,
                                        const char *attempted_tool, const char *partial_text,
                                        const char *last_tool_name, const char *last_tool_result,
                                        int total_tool_calls)
{
   char diag[1800];
   liveness_format_final_tool_call_diagnostic(agent ? agent->name : NULL, attempted_tool,
                                              partial_text, last_tool_name, last_tool_result,
                                              total_tool_calls, diag, sizeof(diag));

   agent_result_clear_response(out);
   out->response = safe_strdup(diag);
   out->success = 0;
   out->abstained = 1;
   snprintf(out->abstain_reason, sizeof(out->abstain_reason),
            "final response synthesized after disabled tool call");
   snprintf(out->error, sizeof(out->error), "model attempted %s on the forced final response turn",
            attempted_tool && attempted_tool[0] ? attempted_tool : "a tool call");
}

/* db1 agent_jobs heartbeat — defined in src/db1/agent_jobs.c. Forward
 * declarations match db1/agent_jobs.h to avoid pulling the db1 umbrella
 * header through the runtime's already-large include set. */
void db1_agent_job_update(int job_id, const char *status, int cursor_turn, const char *result);
void db1_agent_job_heartbeat(int job_id);
void db1_agent_job_heartbeat_ext(int job_id, const char *current_tool, int api_call_count);
int db1_agent_job_is_cancelled(int job_id);
int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz);

/* The agent loop declares a local `char session_id[128]` for the ephemeral-SSH id,
 * which shadows the global session_id() accessor inside that function. Reach the
 * conversation id through this alias rather than renaming a buffer the SSH paths use. */
static const char *econ_conversation_id(void)
{
   return session_id();
}
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

/* Apply the SAFE contract at the authenticated local-tool completion boundary.
 * The result has not crossed a provider boundary yet. The Go economizer removes
 * only RFC 8259 whitespace outside strings and succeeds only when strictly
 * shorter; malformed/non-JSON output remains byte-identical. */
static char *agent_economize_fresh_tool_result(char *result)
{
   if (!result)
      return NULL;
   econ_preset_t preset;
   econ_preset_current(&preset);
   if (!preset.json_compact)
      return result;

   uint8_t *compacted = NULL;
   size_t compacted_len = 0;
   if (econ_module_json_compact(result, strlen(result), &compacted, &compacted_len) != 0)
      return result;
   (void)compacted_len; /* output is NUL-terminated for the existing tool-result surface */
   free(result);
   return (char *)compacted;
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

static int agent_progress_tool_is_mutation(const char *name)
{
   if (!name)
      return 0;
   return strcmp(name, "write_file") == 0 || strcmp(name, "edit_file") == 0 ||
          strcmp(name, "edit_symbol") == 0 || strcmp(name, "apply_patch") == 0;
}

/* Convert a tool call into the stable identity consumed by the dependency-free
 * liveness state machine. read_file receives range-aware treatment; other
 * retrievals use their canonical argument JSON, which still catches a call
 * revisited after unrelated work. */
static void agent_progress_retrieval(const char *name, const char *arguments, char *target,
                                     size_t target_sz, int *range_start, int *range_end,
                                     int *has_range)
{
   if (target && target_sz)
      target[0] = '\0';
   if (range_start)
      *range_start = 0;
   if (range_end)
      *range_end = 0;
   if (has_range)
      *has_range = 0;
   if (!name || !arguments || !target || target_sz == 0)
      return;

   if (strcmp(name, "read_file") == 0)
   {
      cJSON *root = cJSON_Parse(arguments);
      if (root)
      {
         cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");
         cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
         cJSON *limit = cJSON_GetObjectItemCaseSensitive(root, "limit");
         if (cJSON_IsString(path) && path->valuestring)
            snprintf(target, target_sz, "%s", path->valuestring);
         if (cJSON_IsNumber(offset) && cJSON_IsNumber(limit) && limit->valuedouble > 0)
         {
            long long start = offset->valuedouble < 0 ? 0 : (long long)offset->valuedouble;
            long long end = start + (long long)limit->valuedouble - 1;
            if (start > INT_MAX)
               start = INT_MAX;
            if (end > INT_MAX)
               end = INT_MAX;
            if (range_start)
               *range_start = (int)start;
            if (range_end)
               *range_end = (int)end;
            if (has_range)
               *has_range = 1;
         }
         cJSON_Delete(root);
      }
   }
   if (!target[0])
      snprintf(target, target_sz, "%.255s", arguments);
}

static int agent_bootstrap_repository_evidence(const aimee_request_t *catalog, cJSON *messages,
                                               int turn, int timeout_ms, char *last_tool_name,
                                               size_t last_tool_name_n, char *last_tool_result,
                                               size_t last_tool_result_n)
{
   static const char *const name = "list_files";
   /* Never let a synthetic evidence call fall back to the server process CWD.
    * Delegate execution installs its effective worktree in thread-local command
    * context; a direct roundtable without a checkout must remain ungrounded. */
   const char *worktree = run_cmd_get_cwd();
   if (!worktree || !worktree[0] || !aimee_path_is_absolute(worktree))
      return 0;
   const aimee_tool_t *admitted = aimee_ir_find_function(catalog, NULL, name);
   if (!admitted)
      return 0;
   cJSON *arg_obj = cJSON_CreateObject();
   if (!arg_obj)
      return 0;
   cJSON_AddStringToObject(arg_obj, "path", worktree);
   cJSON *canonical = tool_args_canonicalize(admitted->schema, arg_obj);
   char *args = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   cJSON_Delete(arg_obj);
   if (!args)
      return 0;
   char reason[256] = {0}, effect[32], attempt[64];
   aimee_tool_metadata_t metadata;
   snprintf(attempt, sizeof attempt, "bootstrap-repository:%d", turn);
   if (tool_validate_ir(admitted, args, reason, sizeof reason) != 0 ||
       directive_check_tool(name, args, reason, sizeof reason) != 0 ||
       tool_policy_metadata(name, &metadata, effect, sizeof effect, reason, sizeof reason) != 0 ||
       policy_check_tool_metadata_attempt(name, &metadata, effect, args, attempt, reason,
                                          sizeof reason) != 0)
   {
      aimee_log(LOG_WARN, "agent.evidence", "repository evidence admission refused: %s", reason);
      free(args);
      return 0;
   }
   int is_error = 1;
   char *result = dispatch_tool_call_ir_ctx_status(admitted, args, timeout_ms, &is_error);
   agent_trace_log(0, turn, "tool_call", NULL, name, args, result, NULL);
   if (last_tool_name && last_tool_name_n)
      snprintf(last_tool_name, last_tool_name_n, "%s", name);
   if (last_tool_result && last_tool_result_n)
      snprintf(last_tool_result, last_tool_result_n, "%.500s", result ? result : "");
   int usable = !is_error;
   if (usable)
      agent_session_append_repository_evidence(messages, name, args, result);
   free(result);
   free(args);
   return usable;
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
   /* session_compact stays a pure function of its inputs, so the derivation choice
    * is resolved here rather than read from global config inside the compactor. */
   scfg.from_record = config_compact_from_record();

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
      messages_compact_consecutive(messages);
   }
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

static int agent_execute_provider_session(const provider_cli_adapter_t *adapter,
                                          const agent_t *agent, const agent_network_t *network,
                                          const char *role, const char *system_prompt,
                                          const char *user_prompt, const aimee_request_t *seed,
                                          aimee_response_t *canonical, agent_result_t *out)
{
   provider_cli_cfg_t cfg = {.agent = agent,
                             .network = network,
                             .role = role,
                             .cwd = run_cmd_get_cwd(),
                             .system_prompt = system_prompt,
                             .user_prompt = user_prompt,
                             .canonical_request = seed,
                             .canonical_response = canonical};
   int host = provider_cli_adapter_host_execute(&cfg, out);
   if (host != 1)
      return host;
   /* Standalone adapter fixtures have no server capture. An attested session
    * cannot fall back to an SDK credential HOME or private native history. */
   agent_request_creds_t attestation;
   agent_request_creds_snapshot(&attestation);
   int standalone = !seed && !server_session_log_active() && !attestation.session_id[0] &&
                    !attestation.vault_principal[0] && !attestation.codex_token[0] &&
                    !attestation.codex_account_id[0] && !attestation.codex_registration[0] &&
                    !attestation.claude_token[0] && !attestation.claude_registration[0] &&
                    !attestation.claude_target[0] && !attestation.api_key[0] &&
                    !attestation.api_key_registration[0] && !attestation.api_key_target[0];
   OPENSSL_cleanse(&attestation, sizeof attestation);
   if (standalone)
      return provider_cli_adapter_execute_cfg(adapter, &cfg, out);
   memset(out, 0, sizeof *out);
   snprintf(out->error, sizeof out->error, "canonical %s execution owner unavailable",
            adapter->display_name);
   return AGENT_RC_CONTEXT_REFUSED;
}

static int agent_execute_with_tools_impl(const agent_t *agent, const agent_network_t *network,
                                         const char *role, const char *system_prompt,
                                         const char *user_prompt, int max_tokens,
                                         double temperature, cJSON *initial_messages,
                                         cJSON **updated_messages, agent_result_t *out,
                                         const aimee_request_t *seed,
                                         aimee_response_t *canonical_out);

static int agent_execute_with_tools_internal(const agent_t *agent, const agent_network_t *network,
                                             const char *role, const char *system_prompt,
                                             const char *user_prompt, int max_tokens,
                                             double temperature, cJSON *initial_messages,
                                             cJSON **updated_messages, agent_result_t *out,
                                             const aimee_request_t *seed,
                                             aimee_response_t *canonical_out)
{
   int previous = request_context_set_model_execution(1);
   aimee_response_t final = {0};
   int rc = agent_execute_with_tools_impl(agent, network, role, system_prompt, user_prompt,
                                          max_tokens, temperature, initial_messages,
                                          updated_messages, out, seed, &final);
   if (!rc && out->success)
   {
      /* Native/nested owners may already retain their original response. */
      if (final.role || final.n_content || final.error_message)
      {
         aimee_response_free(&out->canonical_response);
         out->canonical_response = final;
         final = (aimee_response_t){0};
      }
      if (canonical_out && aimee_ir_response_clone(&out->canonical_response, canonical_out))
      {
         out->success = 0;
         snprintf(out->error, sizeof out->error, "cannot retain owned canonical execution output");
         aimee_ir_session_fail_capture();
         rc = AGENT_RC_CAPTURE_FAILED;
      }
   }
   aimee_response_free(&final);
   if (previous >= 0)
      (void)request_context_set_model_execution(previous);
   return rc;
}

int agent_execute_with_tools(const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out)
{
   return agent_execute_with_tools_internal(agent, network, NULL, system_prompt, user_prompt,
                                            max_tokens, temperature, NULL, NULL, out, NULL, NULL);
}

int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature,
                                      agent_result_t *out)
{
   if (!server_session_log_active())
      return agent_execute_with_tools_internal(agent, network, role, system_prompt, user_prompt,
                                               max_tokens, temperature, NULL, NULL, out, NULL,
                                               NULL);
   cJSON *history = NULL, *updated = NULL;
   if (server_session_log_history_load(&history) < 0)
   {
      memset(out, 0, sizeof *out);
      snprintf(out->error, sizeof out->error, "canonical execution history is unavailable");
      snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
      aimee_ir_session_fail_capture();
      return AGENT_RC_CAPTURE_FAILED;
   }
   int rc = agent_execute_with_tools_internal(agent, network, role, system_prompt, user_prompt,
                                              max_tokens, temperature, history, &updated, out, NULL,
                                              NULL);
   cJSON_Delete(history);
   if (rc == 0 && updated && server_session_log_history_save(updated))
   {
      rc = AGENT_RC_CAPTURE_FAILED;
      out->success = 0;
      snprintf(out->error, sizeof out->error, "canonical execution checkpoint failed");
      snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
   }
   cJSON_Delete(updated);
   return rc;
}

int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system_prompt, const char *user_prompt,
                                     int max_tokens, double temperature, cJSON *initial_messages,
                                     cJSON **updated_messages, agent_result_t *out)
{
   return agent_execute_with_tools_internal(agent, network, NULL, system_prompt, user_prompt,
                                            max_tokens, temperature, initial_messages,
                                            updated_messages, out, NULL, NULL);
}

int agent_execute_request_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *role, const aimee_request_t *request,
                                     aimee_response_t *response, agent_result_t *out)
{
   if (!agent || !request || !response || !out)
      return -1;
   *response = (aimee_response_t){0};
   memset(out, 0, sizeof *out);
   /* Runs own the server catalog; unsupported client policy is explicit. */
   if (!agent_request_with_tools_supported(request) ||
       (is_chatgpt_provider(agent) && (request->has_max_tokens || request->has_temperature)))
   {
      snprintf(out->error, sizeof out->error,
               "run request contains unsupported execution controls");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   cJSON *messages = cJSON_CreateArray(), *updated = NULL;
   if (!messages)
      return -1;
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = aimee_ir_message_json(&request->messages[i]);
      if (!message || !cJSON_AddItemToArray(messages, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(messages);
         return -1;
      }
   }
   char query[4096];
   aimee_ir_last_user_text(request, query, sizeof query);
   int rc = agent_execute_with_tools_internal(
       agent, network, role, request->n_system ? request->system[0].text : NULL,
       query[0] ? query : "Continue the admitted conversation.",
       request->has_max_tokens ? request->max_tokens : 0,
       request->has_temperature ? request->temperature : 0.3, messages, &updated, out, request,
       response);
   if (!rc && (!updated || server_session_log_history_save(updated)))
   {
      out->success = 0;
      snprintf(out->error, sizeof out->error, "canonical run checkpoint failed");
      rc = AGENT_RC_CAPTURE_FAILED;
   }
   if (rc)
      aimee_response_free(response);
   cJSON_Delete(updated);
   cJSON_Delete(messages);
   return rc;
}

static int agent_execute_with_tools_impl(const agent_t *agent, const agent_network_t *network,
                                         const char *role, const char *system_prompt,
                                         const char *user_prompt, int max_tokens,
                                         double temperature, cJSON *initial_messages,
                                         cJSON **updated_messages, agent_result_t *out,
                                         const aimee_request_t *seed,
                                         aimee_response_t *canonical_out)
{
   if (updated_messages)
      *updated_messages = NULL;
   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);
   snprintf(out->model, MAX_MODEL_LEN, "%s", agent->model);
   snprintf(out->served_model, MAX_MODEL_LEN, "%s", agent->model);
   agent_t native_provider_agent;

   /* Historical tmux configurations are admitted as headless provider adapters.
    * There is no terminal-screen producer in the execution engine. */
   agent_t legacy_provider_agent;
   if (strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0)
   {
      legacy_provider_agent = *agent;
      snprintf(legacy_provider_agent.backend, sizeof legacy_provider_agent.backend, "%s",
               AGENT_BACKEND_PROVIDER_CLI);
      const char *kind = agent->cli_kind[0] ? agent->cli_kind : agent->provider;
      if (!strcmp(kind, "claude-code") || !strcmp(kind, "claude-oauth"))
         kind = "claude";
      snprintf(legacy_provider_agent.cli_kind, sizeof legacy_provider_agent.cli_kind, "%s", kind);
      agent = &legacy_provider_agent;
   }

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
      {
         int owned_native =
             !strcmp(adapter->cli_kind, "codex") || !strcmp(adapter->cli_kind, "claude");
         /* The server-owned SDK pipeline sends model traffic only through its
          * attested canonical gateway, where Go admits the actual rendered
          * bytes. It is not an unobservable external transport. Launch/source
          * qualification still belongs to the host executor, without fallback. */
         int canonical_host =
             owned_native && server_session_log_active() && server_session_capture_current();
         if (!canonical_host && wire_fence_external_backend() != 0)
         {
            snprintf(out->error, sizeof(out->error), "%s", wire_fence_last_error());
            return -1;
         }
         if (seed &&
             (!owned_native || !server_session_log_active() || !server_session_capture_current() ||
              aimee_ir_history_admit(initial_messages) ||
              server_session_log_history_save(initial_messages)))
         {
            snprintf(out->error, sizeof out->error,
                     "canonical seeded execution history is unavailable");
            return AGENT_RC_CONTEXT_REFUSED;
         }
         int rc = agent_execute_provider_session(adapter, agent, network, role, system_prompt,
                                                 user_prompt, seed, canonical_out, out);
         if (!rc && seed && updated_messages &&
             (server_session_capture_history(server_session_capture_current(), updated_messages) ||
              aimee_ir_history_admit(*updated_messages)))
         {
            out->success = 0;
            snprintf(out->error, sizeof out->error, "canonical native run result is unavailable");
            aimee_ir_session_fail_capture();
            rc = AGENT_RC_CAPTURE_FAILED;
         }
         return rc;
      }
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
   int configured_total_timeout_ms = agent_loop_total_timeout_ms(agent->timeout_ms, 0);
   int total_timeout_ms =
       agent_loop_total_timeout_ms(agent->timeout_ms, agent->tool_loop_timeout_ms_cap);

   /* A stage cap can be SMALLER than one viable call, and nothing checked that.
    * The loop then stops on its first iteration and reports the budget as
    * exhausted after a few milliseconds. Seen in production as:
    *
    *   tool loop budget exhausted (elapsed=97ms effective=57759ms
    *                               configured=720000ms stage_remaining_cap=57759ms)
    *
    * Nothing was exhausted -- 57.7s never covered the 60s minimum call, so the
    * delegate could not have started. Refusing here says that plainly instead of
    * blaming a budget the turn never got to spend, and it costs no model call.
    * The roundtable path already preflights its phase budget this way. */
   if (agent_loop_window_too_small(agent->timeout_ms, total_timeout_ms))
   {
      int minimum =
          agent->timeout_ms < AGENT_LOOP_MIN_CALL_MS ? agent->timeout_ms : AGENT_LOOP_MIN_CALL_MS;
      snprintf(out->error, sizeof(out->error),
               "stage window too small to start a delegate call (effective=%dms minimum=%dms "
               "configured=%dms stage_remaining_cap=%dms)",
               total_timeout_ms, minimum, configured_total_timeout_ms,
               agent->tool_loop_timeout_ms_cap);
      return -1;
   }

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
   /* Read, not derived: the permission was resolved when the run was configured. */
   int current_code_only = !agent_tools_knowledge_write_allowed();
   char *task_instructions =
       current_code_only ? NULL : ingress_preinject_task_instructions(system_prompt, user_prompt);
   if (task_instructions)
      system_prompt = task_instructions;
   char *assembled_sys = agent_build_exec_context_checked(
       agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL), role, system_prompt,
       current_code_only, out->error, sizeof(out->error));
   if (!assembled_sys)
   {
      free(task_instructions);
      snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      return AGENT_RC_CONTEXT_REFUSED;
   }
   int context_refused = 0;
   int capture_failed = 0;
   const char *sys = assembled_sys;
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
   char auth_header[MAX_API_KEY_LEN + 32], extra_headers[512];
   if (agent_resolve_auth_headers(agent, auth_header, sizeof auth_header, extra_headers,
                                  sizeof extra_headers) != 0)
   {
      snprintf(out->error, sizeof(out->error), "auth resolution failed");
      free(assembled_sys);
      free(task_instructions);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      return -1;
   }

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
      free(assembled_sys);
      free(task_instructions);
      return -1;
   }

   /* Build tools JSON (reused each turn, format depends on provider) */
   cJSON *tools = chatgpt     ? build_tools_array_responses()
                  : anthropic ? build_tools_array_anthropic()
                              : build_tools_array(); /* OpenAI format */
   if (!tools)
   {
      free(assembled_sys);
      free(task_instructions);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      snprintf(out->error, sizeof out->error, "tool catalog could not be admitted as IR");
      return -1;
   }
   if (anthropic)
      agent_tools_filter_anthropic_for_role(tools, role);
   else
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

   /* Legacy transcripts cross an explicit entry adapter once. Every message
    * admitted after this point is canonical {role,blocks}, including host text. */
   aimee_wire_t wire = chatgpt     ? AIMEE_WIRE_RESPONSES
                       : anthropic ? AIMEE_WIRE_ANTHROPIC
                                   : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *messages = aimee_frontend_history_import(initial_messages, wire);
   if (!messages)
   {
      cJSON_Delete(tools);
      agent_tools_set_dispatch_role(NULL);
      free(assembled_sys);
      free(task_instructions);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      snprintf(out->error, sizeof out->error, "session history could not be admitted as IR");
      return -1;
   }
   /* System context is assembled per request, never persisted in the session. */
   cJSON *user_msg = seed ? NULL : agent_session_text_message("user", user_prompt);
   if ((!seed && (!user_msg || !cJSON_AddItemToArray(messages, user_msg))) ||
       (seed && server_session_log_history_save(messages)))
   {
      cJSON_Delete(user_msg);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      agent_tools_set_dispatch_role(NULL);
      free(assembled_sys);
      free(task_instructions);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      snprintf(out->error, sizeof out->error, "cannot retain user message IR");
      return -1;
   }

   aimee_request_t dispatch_catalog = {0};
   int turn = 0;
   int total_calls = 0;
   int successful_tool_calls = 0;
   long canonical_usage[5] = {0};
   int api_call_count = 0; /* cumulative provider API calls; published via heartbeat */
   int final_instruction_added = 0;
   int final_tool_retry_count = 0;
   int degenerate_retry_count = 0;
   int required_evidence_retry_count = 0;
   int required_evidence_responses = 0;
   /* A provider may emit a reasoning-only/empty turn while tools are offered
    * even though it has finished investigating (observed with Codex review
    * seats). Retrying with the identical tool contract can repeat forever and
    * discard an otherwise healthy panelist. Preserve tools for the normal run,
    * but make the one bounded degenerate-response retry a synthesis-only turn. */
   int tok = agent_request_max_tokens(agent, max_tokens);
   /* Turn-budget policy keys off the BUDGET role, not the routing role: the
    * primary webchat turn is routed as "code" (server_compute.c) but must be
    * budgeted as a primary session — uncapped, and never told its tool budget is
    * exhausted. `budget_role` is NULL exactly when this is a primary turn. */
   const char *budget_role = agent_budget_role(role);
   int max_t = agent_resolve_max_turns(agent, budget_role);
   int initial_max_t = max_t;
   int final_after_turns = delegate_final_after_turns_for_role(budget_role);

   /* Stuck detection state */
   char last_tool_sig[256] = {0};
   char last_tool_name[64] = {0};
   char last_tool_result[512] = {0};
   int repeat_count = 0;
   int total_repeat_triggers =
       0; /* circuit breaker: aborts after LIVENESS_REPEAT_ABORT_THRESHOLD */
   liveness_progress_state_t progress;
   liveness_progress_init(&progress);
   int progress_enabled = delegate_role_is_write(budget_role);
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

   int mw_max_turns = budget_role ? max_t : 0;
   mw_pipeline_t mw_pipeline;
   mw_pipeline_cfgs_t mw_cfgs;
   mw_pipeline_build(&mw_pipeline, &mw_cfgs, &agent->middleware, mw_max_turns, agent->model);

   while (turn < max_t)
   {
      /* Stop before issuing a fourth provider request without evidence. The
       * third response is allowed to execute its calls; if none succeeds, the
       * next iteration fails closed here. */
      if (agent_required_evidence_budget_exhausted(
              agent->require_initial_tool_call, successful_tool_calls, required_evidence_responses))
      {
         snprintf(out->error, sizeof(out->error),
                  "repository evidence not obtained within %d provider responses",
                  AGENT_REQUIRED_EVIDENCE_RETRY_LIMIT + 1);
         break;
      }
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
         if (agent->tool_loop_timeout_ms_cap > 0)
            snprintf(out->error, sizeof(out->error),
                     "tool loop total timeout exceeded (elapsed=%dms effective=%dms "
                     "configured=%dms stage_remaining_cap=%dms)",
                     elapsed_ms, total_timeout_ms, configured_total_timeout_ms,
                     agent->tool_loop_timeout_ms_cap);
         else
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

         mw_pipeline_cfgs_set_max_turns(&mw_cfgs, budget_role ? max_t : 0);
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
            session_compact_config_t mw_scfg;
            memset(&mw_scfg, 0, sizeof(mw_scfg));
            mw_scfg.from_record = config_compact_from_record();
            session_compact(messages, &mw_scfg, &sc_result);
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
               /* A failed compaction leaves the admitted source unchanged. */
               messages_compact_consecutive(messages);
            }
            /* fall through: continue with the compacted history */
         }

         if (mw_res.action == MW_INJECT && mw_res.message[0])
         {
            cJSON *inject_ir = agent_session_text_message("user", mw_res.message);
            if (inject_ir)
               cJSON_AddItemToArray(messages, inject_ir);
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
            char *refreshed = agent_build_exec_context_checked(
                agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL), role,
                system_prompt, current_code_only, out->error, sizeof(out->error));
            if (!refreshed)
            {
               context_refused = 1;
               out->success = 0;
               snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
               break;
            }
            if (refreshed)
            {
               free(assembled_sys);
               assembled_sys = refreshed;
               sys = assembled_sys;
            }
         }
      }

      /* Execution cannot invent missing receipts or silently discard effects. */
      if (aimee_ir_history_admit(messages))
      {
         snprintf(out->error, sizeof out->error, "canonical history is incomplete or invalid");
         break;
      }
      messages_compact_consecutive(messages);

      maybe_compact_before_request(&fb_agent, messages, sys);

      /* Primary sessions never close the tool budget — the user can always send
       * another message.  budget_role is NULL for them (including the webchat
       * turn, which is ROUTED as "code"), so max_turns=0 and HARD never fires. */
      liveness_final_response_mode_t final_mode = liveness_final_response_mode(
          turn, budget_role ? max_t : 0, total_calls, final_after_turns);
      int final_instruction_turn = final_mode != LIVENESS_FINAL_RESPONSE_NONE;
      int final_text_only_turn = !liveness_final_response_allows_tools(final_mode);
      /* A final-only turn cannot satisfy an evidence gate. Keep read-only tools
       * available until one actually succeeds; provider tool_choice is a request,
       * not an enforcement boundary, and some compatible endpoints ignore it. */
      if (agent_evidence_gate_defers_final_turn(agent->require_initial_tool_call,
                                                successful_tool_calls,
                                                final_mode == LIVENESS_FINAL_RESPONSE_HARD))
      {
         final_instruction_turn = 0;
         final_text_only_turn = 0;
      }
      if (final_instruction_turn && !final_instruction_added)
      {
         agent_session_append_final_instruction(messages);
         final_instruction_added = 1;
      }
      cJSON *active_tools = final_text_only_turn ? NULL : tools;
      /* Evidence-gated review can require one real repository lookup. Force
       * provider tool selection only until the first successful tool call;
       * leaving it required would prevent the final text turn forever. */
      fb_agent.require_initial_tool_call = agent_require_initial_tool_choice(
          agent->require_initial_tool_call, successful_tool_calls, active_tools != NULL);

      /* AGGRESSIVE opts into the existing lossy context reducers on OpenAI-family
       * routes. SAFE never touches previously sent history, and native Anthropic
       * history is never reduced because its exact prefix controls cache reuse. */
      /* Context reduction now lives in the Go economizer module and is reached
       * over the event bus (stage economizer-reduce, kind 11009). This side
       * resolves config and persists state; every decision is the module's.
       *
       * FAIL-OPEN: an unreachable module leaves `reduced` NULL and the turn runs
       * on its original context. Losing the economizer costs tokens; failing the
       * turn costs the user's work. */
      cJSON *reduced_messages = NULL;
      econ_module_result_t econ_res;
      memset(&econ_res, 0, sizeof econ_res);
      int reduce_active = 0;
      {
         econ_preset_t preset;
         econ_preset_current(&preset);
         char closet_denylist[CONFIG_COPY_MAX];
         config_coord_closet_denylist_copy(closet_denylist, sizeof(closet_denylist));
         if (!anthropic && (preset.history_fold || preset.compress))
         {
            econ_module_request_t mreq;
            memset(&mreq, 0, sizeof mreq);
            /* The fold is reachable on its own, not only via the AGGRESSIVE
             * preset: fold.enabled had zero live callers before, so the only way
             * to turn it on was a tier that also enables wire mutation.
             *
             * THE CHATGPT ROUTE FOLDS TOO. It was excluded in #913 as "unverified"
             * rather than as known-broken, and the exclusion then outlived the
             * doubt that motivated it: chatgpt is what a codex-provider deployment
             * actually uses, so excluding it meant the fold never ran there at all.
             * Measured on CT 403 with the module attached and mode=aggressive, the
             * same request cost an IDENTICAL 38826 prompt tokens with the
             * economizer attached and detached — the lever was structurally
             * unreachable on the only route the box uses.
             *
             * The original doubt was about SHAPE: folded turns are {role, content}
             * with a string content, and agent_build_request_responses forwards
             * `input` without converting to typed items. The Responses API accepts
             * that string shorthand, which is why this is safe, and it is verified
             * live against the real backend rather than argued from the spec. */
            mreq.history_fold = preset.history_fold || config_fold_enabled();
            mreq.compress = preset.compress;
            mreq.freeze_guard_enabled = 1;
            mreq.freeze_guard_horizon = preset.freeze_guard_horizon;
            mreq.retained_msgs = config_fold_retained_msgs();
            mreq.min_fold_msgs = config_fold_min_fold_msgs();
            mreq.excerpt_bytes = config_fold_excerpt_bytes();
            mreq.compact_head_bytes = config_compact_head_bytes();
            mreq.compact_tail_bytes = config_compact_tail_bytes();
            mreq.register_enabled = config_fold_register_enabled();
            mreq.closet_enabled = config_coord_closet_enabled();
            mreq.closet_budget_bytes = config_coord_closet_budget_bytes();
            mreq.closet_max_ratio_pct = config_coord_closet_max_ratio_pct();
            mreq.closet_denylist = closet_denylist[0] ? closet_denylist : NULL;
            mreq.recall_enabled = config_fold_recall_enabled();
            mreq.recall_ttl_turns = config_fold_recall_ttl_turns();
            mreq.recall_inject = config_fold_recall_inject();
            /* Name the conversation and let the module keep its own reducer
             * state under it. A run is one agent invocation but a conversation
             * spans several, so without a stable name every user turn would
             * re-derive the fold boundary and start with an EMPTY page table --
             * a coordinate evicted in the previous run would be unrecoverable.
             *
             * Strictly the session id, and NULL when there is none: a shared or
             * guessed name here would serve one conversation another's context. */
            mreq.state_key = econ_conversation_id();

            /* The freeze guardrail prices each cache tier; the module does the
             * arithmetic, this side supplies the rates. */
            {
               token_usage_t w = {0}, in = {0}, rd = {0};
               w.cache_write_tokens = 1000;
               in.input_tokens = 1000;
               rd.cache_read_tokens = 1000;
               int priced = 0;
               double wc = token_estimate_cost_ex(fb_agent.model, &w, &priced);
               if (priced)
               {
                  mreq.priced = 1;
                  mreq.write_cost = wc;
                  mreq.input_cost = token_estimate_cost_ex(fb_agent.model, &in, NULL);
                  mreq.read_cost = token_estimate_cost_ex(fb_agent.model, &rd, NULL);
               }
            }

            int econ_rc = econ_module_reduce(messages, sys, ECON_MODULE_SEAM_DELEGATE, &mreq,
                                             &reduced_messages, &econ_res);
            if (econ_rc == 0 && econ_res.mutated && reduced_messages)
               reduce_active = 1;
            /* Same line the gateway seam emits, for the same reason: the delegate
             * seam recorded NOTHING about what a reduction saved.
             * agent_record_reduce_ledger was deleted as having no callers and
             * nothing replaced it, so the only way to tell whether the fold had run
             * was to diff token counts between two deploys. reused=1 means the
             * freeze held and the prefix was byte-identical to last turn. */
            if (econ_rc != 0)
               aimee_log(LOG_INFO, "economizer.delegate",
                         "seam=delegate UNREACHED call_result=%d (1=capability_absent: nothing "
                         "is serving the reduce stage) -- turn ran on its original context",
                         econ_res.call_result);
            else
               aimee_log(LOG_INFO, "economizer.delegate",
                         "seam=delegate mutated=%d reason=%s baseline=%d reduced=%d removed=%d "
                         "folded=%d retained=%d reused=%d",
                         econ_res.mutated, econ_res.reason[0] ? econ_res.reason : "none",
                         econ_res.baseline_tokens, econ_res.reduced_tokens, econ_res.removed_tokens,
                         econ_res.folded_msgs, econ_res.retained_msgs, econ_res.reused_boundary);
         }
      }
      cJSON *eff_messages = reduce_active ? reduced_messages : messages;

      /* Build request (use fb_agent which may have fallback model after turn 0) */
      cJSON *req;
      if (chatgpt)
         req = agent_build_request_ir(&fb_agent, eff_messages, active_tools, sys, tok, temperature,
                                      AIMEE_WIRE_RESPONSES, role != NULL, &dispatch_catalog);
      else if (anthropic)
      {
         track_anthropic_payload_rewrite(driver, &fb_agent, eff_messages, sys);
         req = agent_build_request_ir(&fb_agent, eff_messages, active_tools, sys, tok, temperature,
                                      AIMEE_WIRE_ANTHROPIC, role != NULL, &dispatch_catalog);
      }
      else
         req = agent_build_request_ir(&fb_agent, eff_messages, active_tools, sys, tok, temperature,
                                      AIMEE_WIRE_OPENAI_CHAT, role != NULL, &dispatch_catalog);

      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      cJSON_Delete(reduced_messages);
      econ_module_result_free(&econ_res);
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
         if (agent->tool_loop_timeout_ms_cap > 0)
            snprintf(out->error, sizeof(out->error),
                     "tool loop budget exhausted (elapsed=%dms effective=%dms configured=%dms "
                     "stage_remaining_cap=%dms)",
                     elapsed_ms, total_timeout_ms, configured_total_timeout_ms,
                     agent->tool_loop_timeout_ms_cap);
         else
            snprintf(out->error, sizeof(out->error),
                     "tool loop budget exhausted (%dms of %dms used)", elapsed_ms,
                     total_timeout_ms);
         free(body);
         break;
      }
      int ra =
          config_retry_max_attempts() > 0 ? config_retry_max_attempts() : HTTP_RETRY_MAX_ATTEMPTS;
      int rb = config_retry_base_ms() > 0 ? config_retry_base_ms() : HTTP_RETRY_BASE_MS;
      int rm = config_retry_max_ms() > 0 ? config_retry_max_ms() : HTTP_RETRY_MAX_MS;

      {
         int dj = agent_get_durable_job_id();
         if (dj > 0)
            db1_agent_job_heartbeat_ext(dj, final_text_only_turn ? "final_response" : "model",
                                        api_call_count);
      }
      int economizer_active = config_present() && econ_mode_current() != ECON_MODE_OFF;
      wire_fence_route_t wire_route = anthropic                   ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                      : chatgpt                   ? WIRE_FENCE_OPENAI_RESPONSES
                                      : strstr(url, "/responses") ? WIRE_FENCE_OPENAI_RESPONSES
                                                                  : WIRE_FENCE_OPENAI_CHAT;
      wire_fence_t *wire_snapshot = NULL;
      wire_fence_bytes_t wire_body;
      int wire_rc = wire_fence_select(economizer_active, wire_route, body, strlen(body),
                                      &wire_snapshot, &wire_body);
      if (wire_rc != 0)
      {
         context_refused = wire_rc == WIRE_FENCE_CONTEXT_REFUSED;
         if (context_refused)
            snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
         snprintf(out->error, sizeof(out->error), "%s", wire_fence_last_error());
         free(body);
         break;
      }
      aimee_anthropic_replay_t sent_replay = {0};
      if (anthropic && aimee_anthropic_replay_prepare(wire_body.data, wire_body.len, &sent_replay))
      {
         capture_failed = 1;
         aimee_ir_session_fail_capture();
         snprintf(out->error, sizeof out->error, "cannot bind native replay prefix");
         free(body);
         wire_fence_destroy(wire_snapshot);
         break;
      }
      int http_status = wire_fence_post(url, auth_header, wire_body.data, wire_body.len,
                                        &response_body, per_call, extra_headers, ra, rb, rm,
                                        agent->provider, fb_agent.model, session_id, wire_route);
      api_call_count++;
      {
         int dj = agent_get_durable_job_id();
         if (dj > 0)
            db1_agent_job_heartbeat_ext(dj, "", api_call_count);
      }
      free(body);
      wire_fence_destroy(wire_snapshot);

      if (http_status == HTTP_RETRY_ADMISSION_REFUSED)
      {
         context_refused = 1;
         snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
         snprintf(out->error, sizeof(out->error), "%s", wire_fence_last_error());
         break;
      }

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
         if (chatgpt)
            fb_req =
                agent_build_request_ir(&fb_agent, messages, active_tools, sys, tok, temperature,
                                       AIMEE_WIRE_RESPONSES, role != NULL, &dispatch_catalog);
         else if (anthropic)
         {
            track_anthropic_payload_rewrite(driver, &fb_agent, messages, sys);
            fb_req =
                agent_build_request_ir(&fb_agent, messages, active_tools, sys, tok, temperature,
                                       AIMEE_WIRE_ANTHROPIC, role != NULL, &dispatch_catalog);
         }
         else
            fb_req =
                agent_build_request_ir(&fb_agent, messages, active_tools, sys, tok, temperature,
                                       AIMEE_WIRE_OPENAI_CHAT, role != NULL, &dispatch_catalog);
         char *fb_body = cJSON_PrintUnformatted(fb_req);
         cJSON_Delete(fb_req);
         if (fb_body)
         {
            {
               int dj = agent_get_durable_job_id();
               if (dj > 0)
                  db1_agent_job_heartbeat_ext(dj, "model", api_call_count);
            }
            wire_fence_t *fb_snapshot = NULL;
            wire_fence_bytes_t fb_wire_body;
            wire_rc = wire_fence_select(economizer_active, wire_route, fb_body, strlen(fb_body),
                                        &fb_snapshot, &fb_wire_body);
            if (wire_rc != 0)
            {
               context_refused = wire_rc == WIRE_FENCE_CONTEXT_REFUSED;
               if (context_refused)
                  snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
               snprintf(out->error, sizeof(out->error), "%s", wire_fence_last_error());
               free(fb_body);
               break;
            }
            if (anthropic &&
                aimee_anthropic_replay_prepare(fb_wire_body.data, fb_wire_body.len, &sent_replay))
            {
               capture_failed = 1;
               aimee_ir_session_fail_capture();
               snprintf(out->error, sizeof out->error, "cannot bind fallback native replay prefix");
               free(fb_body);
               wire_fence_destroy(fb_snapshot);
               break;
            }
            http_status =
                wire_fence_post(url, auth_header, fb_wire_body.data, fb_wire_body.len,
                                &response_body, per_call, extra_headers, ra, rb, rm,
                                fb_agent.provider, fb_agent.model, session_id, wire_route);
            api_call_count++;
            {
               int dj = agent_get_durable_job_id();
               if (dj > 0)
                  db1_agent_job_heartbeat_ext(dj, "", api_call_count);
            }
            free(fb_body);
            wire_fence_destroy(fb_snapshot);
         }
      }

      if (http_status == HTTP_RETRY_ADMISSION_REFUSED)
      {
         context_refused = 1;
         snprintf(out->stop_reason, sizeof(out->stop_reason), "context_refused");
         snprintf(out->error, sizeof(out->error), "%s", wire_fence_last_error());
         break;
      }

      /* Update provider health cache */
      provider_health_update(agent->registration[0] ? agent->registration : agent->name,
                             http_status);

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

      /* Native output ends here. Execution retains the owned typed response;
       * text and argument strings below are views for existing consumer APIs. */
      aimee_response_t response = {0};
      char *response_text = NULL;
      char **tool_arguments = NULL;
      int rescue_enabled = !fb_agent.ablation.configured || fb_agent.ablation.rescue;
      int rescue_mode = rescue_enabled ? agent_allows_json_content_rescue(&fb_agent) : -1;
      int n_rescued = 0;
      int decode_rc;
      if (chatgpt)
         decode_rc = agent_ir_decode_responses(response_body, rescue_mode, &n_rescued, &response);
      else
      {
         cJSON *root = cJSON_Parse(response_body);
         decode_rc =
             root ? agent_ir_decode_json_response_bound(root, anthropic, rescue_mode, &n_rescued,
                                                        anthropic ? &sent_replay : NULL, &response)
                  : -1;
         cJSON_Delete(root);
      }
      free(response_body);
      if (decode_rc != 0)
      {
         capture_failed = decode_rc == AGENT_IR_CAPTURE_FAILED || aimee_ir_session_capture_failed();
         snprintf(out->error, sizeof(out->error), "%s",
                  decode_rc == AGENT_IR_CAPTURE_FAILED
                      ? "canonical response persistence failed"
                      : "provider response could not be admitted as IR");
         if (capture_failed)
            snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
         break;
      }
      if (response.error_message)
      {
         snprintf(out->error, sizeof out->error, "%s", response.error_message);
         snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
                  aimee_stop_reason_name(response.stop_reason));
         aimee_response_free(&response);
         break;
      }
      snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
               aimee_stop_reason_name(response.stop_reason));
      int call_count = aimee_ir_response_tool_count(&response);
      /* Universal-gateway P4 (response side): ask the event-bus governance module
       * about a denied tool_use the model emitted anyway. Shape-agnostic — the
       * returned decision is applied to the typed IR blocks. This stays
       * gated to delegate calls: the primary spawns delegates via the very
       * Task/Subagent tool this policy would drop, so applying it to the primary's
       * response would neuter aimee's own delegation. */
      int denied_calls = 0;
      char denied_tool[64] = "";
      if (role != NULL)
      {
         int governance_tri = config_present() ? config_module_governance() : -1;
         denied_calls = agent_response_govern(
             &response, config_module_enabled(governance_tri, gw_response_governance_enabled()),
             gateway_prevent_subagents_enabled(), denied_tool, sizeof denied_tool);
         call_count = aimee_ir_response_tool_count(&response);
         if (denied_calls < 0)
         {
            delegate_response_release(&response, NULL, NULL, 0);
            delegate_capture_stop(out, &capture_failed);
            snprintf(out->error, sizeof out->error, "cannot persist host governance decision");
            break;
         }
      }
      response_text = aimee_ir_response_text_alloc(&response);
      if (!response_text)
      {
         delegate_response_release(&response, NULL, tool_arguments, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof(out->error), "cannot materialize response text");
         break;
      }

      /* Log response trace */
      agent_trace_log(0, turn, "response", response_text ? response_text : "(tool_calls)", NULL,
                      NULL, NULL, NULL);

      if (agent_required_evidence_keep_tools(agent->require_initial_tool_call,
                                             successful_tool_calls))
         required_evidence_responses++;

      /* ChatGPT can expose provider-native Task/Agent tools outside the function
       * catalog, ignore a named tool_choice, or return a native tool response the
       * wire parser cannot recognize. The gateway still denies identified native
       * delegation calls. When no advertised call survives, perform one bounded,
       * read-only bootstrap from the delegate's explicit worktree and return its
       * output to that same seat. */
      /* This value is intentionally captured after policy compaction. A mixed
       * [denied, allowed] response retains its allowed call and never falls back. */
      int remaining_calls_after_policy = call_count;
      if (agent_required_evidence_needs_fallback(agent->require_initial_tool_call,
                                                 successful_tool_calls, chatgpt,
                                                 remaining_calls_after_policy))
      {
         if (denied_calls > 0)
            aimee_log(LOG_WARN, "agent.evidence",
                      "provider selected denied tool '%s'; bootstrapping seat with list_files",
                      denied_tool[0] ? denied_tool : "Subagent");
         else
            aimee_log(LOG_WARN, "agent.evidence",
                      "provider returned no executable repository call; bootstrapping seat with "
                      "list_files");
         total_calls++;
         if (agent_bootstrap_repository_evidence(
                 &dispatch_catalog, messages, turn, agent->timeout_ms, last_tool_name,
                 sizeof last_tool_name, last_tool_result, sizeof last_tool_result))
         {
            successful_tool_calls++;
            if (turn >= max_t - 1 && max_t < initial_max_t + 1)
               max_t++;
            turn++;
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            continue;
         }
         consecutive_errors++;
      }

      /* Accumulate tokens */
      const long counts[5] = {response.usage_in, response.usage_out, response.usage_cache_write,
                              response.usage_cache_read, response.usage_reasoning};
      const long usage_limit = LONG_MAX > 9007199254740991LL ? 9007199254740991LL : LONG_MAX;
      int usage_overflow = 0;
      for (int i = 0; i < 5; i++)
         if (counts[i] < 0 || counts[i] > usage_limit - canonical_usage[i])
            usage_overflow = 1;
      if (usage_overflow)
      {
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof out->error, "aggregate usage exceeds canonical output range");
         break;
      }
      for (int i = 0; i < 5; i++)
         canonical_usage[i] += counts[i];
      /* Compatibility counters have an int boundary. The typed response and
       * canonical per-call observations retain the complete exact counts. */
      out->prompt_tokens = canonical_usage[0] > INT_MAX ? INT_MAX : (int)canonical_usage[0];
      out->completion_tokens = canonical_usage[1] > INT_MAX ? INT_MAX : (int)canonical_usage[1];
      out->cache_write_tokens = canonical_usage[2] > INT_MAX ? INT_MAX : (int)canonical_usage[2];
      out->cache_read_tokens = canonical_usage[3] > INT_MAX ? INT_MAX : (int)canonical_usage[3];

      /* XML tool-call rescue: models without native function-calling embed
       * <tool_call>...</tool_call> blocks in their text. The entry adapter owns
       * recovery; no downstream native/text parser repeats the conversion. */
      {
         /* A FINAL text-only turn that smuggled a tool call (rescued from prose) is a
          * policy violation: it should have answered, not called a tool. Detected via
          * n_rescued instead of re-scanning the text. */
         if (final_text_only_turn && n_rescued > 0)
         {
            char attempted_tool[256];
            snprintf(attempted_tool, sizeof attempted_tool, "%s",
                     call_count > 0 ? aimee_ir_response_tool(&response, 0)->tool_name
                                    : "XML tool_call block");
            /* If the smuggled call was actually the respond tool, it IS a final
             * answer -- strip it and finish successfully. */
            int stripped = inject_respond_tool ? agent_response_consume_final(&response) : 0;
            if (stripped < 0)
            {
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               delegate_capture_stop(out, &capture_failed);
               snprintf(out->error, sizeof out->error, "cannot retain final response IR");
               break;
            }
            if (stripped)
            {
               call_count = aimee_ir_response_tool_count(&response);
               free(response_text);
               response_text = aimee_ir_response_text_alloc(&response);
            }
            if (stripped == 1 && response_text && !liveness_is_degenerate_response(response_text))
            {
               if (delegate_append_response(messages, &response) != 0)
               {
                  delegate_response_release(&response, response_text, tool_arguments, call_count);
                  delegate_capture_stop(out, &capture_failed);
                  snprintf(out->error, sizeof out->error, "cannot retain final response IR");
                  break;
               }
               snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
                        aimee_stop_reason_name(response.stop_reason));
               out->response = response_text;
               response_text = NULL;
               out->rescue_recoveries++;
               out->success = 1;
               if (canonical_out)
               {
                  *canonical_out = response;
                  response = (aimee_response_t){0};
               }
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               break;
            }
            if (agent_session_retry_final_tool_violation(messages, attempted_tool, &turn, &max_t,
                                                         initial_max_t, &final_tool_retry_count,
                                                         out->error, sizeof(out->error)))
            {
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               continue;
            }
            finish_final_tool_violation(out, agent, attempted_tool, response_text, last_tool_name,
                                        last_tool_result, total_calls);
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            break;
         }
         if (n_rescued > 0)
            out->rescue_recoveries++;
      }
      int respond_strip = inject_respond_tool ? agent_response_consume_final(&response) : 0;
      if (respond_strip < 0)
      {
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof(out->error), "cannot retain final response IR");
         break;
      }
      if (respond_strip)
      {
         call_count = aimee_ir_response_tool_count(&response);
         free(response_text);
         response_text = aimee_ir_response_text_alloc(&response);
         if (!response_text)
         {
            delegate_response_release(&response, NULL, tool_arguments, call_count);
            delegate_capture_stop(out, &capture_failed);
            snprintf(out->error, sizeof out->error, "cannot retain final response text");
            break;
         }
      }

      if (final_text_only_turn && (call_count > 0))
      {
         const char *attempted_tool =
             (call_count > 0 && aimee_ir_response_tool(&response, 0)->tool_name[0])
                 ? aimee_ir_response_tool(&response, 0)->tool_name
                 : "tool call";
         if (agent_session_retry_final_tool_violation(messages, attempted_tool, &turn, &max_t,
                                                      initial_max_t, &final_tool_retry_count,
                                                      out->error, sizeof(out->error)))
         {
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            continue;
         }
         finish_final_tool_violation(out, agent, attempted_tool, response_text, last_tool_name,
                                     last_tool_result, total_calls);
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         break;
      }

      /* Enforce the evidence requirement at the response boundary, not merely in
       * provider request shaping. Providers can ignore tool_choice, and policy
       * policing can remove an unusable call while leaving a nominal tool-call
       * response. Neither is repository evidence. Retry with an explicit
       * instruction, then fail closed after the bounded retry budget. */
      if (agent_required_evidence_reject_response(agent->require_initial_tool_call,
                                                  successful_tool_calls, (call_count > 0),
                                                  call_count))
      {
         if (agent_session_retry_required_evidence(messages, &turn, &max_t, initial_max_t,
                                                   &required_evidence_retry_count, out->error,
                                                   sizeof(out->error)))
         {
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            continue;
         }
         out->success = 0;
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         break;
      }

      if (!(call_count > 0))
      {
         /* Final text response */
         if (response_text)
         {
            int visible_media = 0;
            for (int i = 0; canonical_out && i < response.n_content; i++)
               if (response.content[i].type == AIMEE_BLK_IMAGE ||
                   response.content[i].type == AIMEE_BLK_DOCUMENT)
                  visible_media = 1;
            if (!visible_media && liveness_is_degenerate_response(response_text))
            {
               if (total_calls == 0 && agent_session_retry_degenerate_response(
                                           messages, &turn, &degenerate_retry_count))
               {
                  delegate_response_release(&response, response_text, tool_arguments, call_count);
                  continue;
               }
               /* Empty or punctuation-only response — replace with explicit failure diagnostic.
                */
               char diag[512];
               liveness_format_empty_diagnostic(agent->name, diag, sizeof(diag));
               free(response_text);
               response_text = NULL;
               out->response = safe_strdup(diag);
               out->success = 0;
               snprintf(out->error, sizeof(out->error),
                        "delegate returned empty or degenerate response");
            }
            else
            {
               if (delegate_append_response(messages, &response) != 0)
               {
                  delegate_response_release(&response, response_text, tool_arguments, call_count);
                  delegate_capture_stop(out, &capture_failed);
                  snprintf(out->error, sizeof out->error, "cannot retain final response IR");
                  break;
               }
               snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
                        aimee_stop_reason_name(response.stop_reason));
               out->response = response_text;
               response_text = NULL; /* transfer the text projection to the result API */
               out->success = 1;
            }
         }
         else
         {
            /* No tool call means the provider omitted synthesis entirely; give
             * it the same bounded degenerate-response correction used for
             * reasoning-only content. The final-tool retry below is mutually
             * exclusive because it requires total_calls > 0. */
            if (total_calls == 0 &&
                agent_session_retry_degenerate_response(messages, &turn, &degenerate_retry_count))
            {
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               continue;
            }
            if (total_calls > 0 &&
                agent_session_retry_final_tool_violation(
                    messages, "an empty final response", &turn, &max_t, initial_max_t,
                    &final_tool_retry_count, out->error, sizeof(out->error)))
            {
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               continue;
            }
            out->success = 0;
            snprintf(out->error, sizeof(out->error), "no content in final response");
         }
         if (out->success && canonical_out)
         {
            *canonical_out = response;
            response = (aimee_response_t){0};
         }
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         break;
      }

      /* Execute tool calls */
      {
         char cancel_reason[128];
         if (agent_durable_cancelled(cancel_reason, sizeof(cancel_reason)))
         {
            snprintf(out->error, sizeof(out->error), "%s before tool execution at turn %d",
                     cancel_reason, turn + 1);
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            break;
         }
      }

      if (delegate_append_response(messages, &response) != 0)
      {
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof(out->error), "cannot retain assistant response");
         break;
      }

      /* Notify auto-snapshot context of the current turn so file writes can be grouped */
      agent_tools_begin_turn(turn);

      liveness_progress_action_t progress_action = LIVENESS_PROGRESS_NONE;

      /* Serialization happens only at the string-based tool service boundary.
       * The model's typed input and opaque IDs remain in the response. Every
       * validator, authorizer, executor and audit consumer sees the same bytes. */
      tool_arguments = calloc((size_t)call_count, sizeof *tool_arguments);
      if (!tool_arguments)
      {
         delegate_response_release(&response, response_text, NULL, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof out->error, "cannot retain tool argument views");
         break;
      }
      int arguments_ok = 1;
      for (int i = 0; i < call_count; i++)
      {
         aimee_block_t *tool = aimee_ir_response_tool(&response, i);
         char *original =
             tool->tool_input ? cJSON_PrintUnformatted(tool->tool_input) : safe_strdup("{}");
         const aimee_tool_t *advertised =
             aimee_ir_find_function(&dispatch_catalog, tool->tool_namespace, tool->tool_name);
         tool_arguments[i] = original && advertised
                                 ? tool_args_canonicalize_json(advertised->schema, original)
                                 : NULL;
         if (!tool_arguments[i])
            tool_arguments[i] = original;
         else
            free(original);
         if (!tool_arguments[i])
            arguments_ok = 0;
      }
      if (!arguments_ok)
      {
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         delegate_capture_stop(out, &capture_failed);
         snprintf(out->error, sizeof(out->error), "cannot serialize typed tool arguments");
         break;
      }

      /* Rounds-to-resume accounting: judge what the model ASKED for, before the
       * tools run — a re-derivation is a re-derivation whether or not it
       * succeeds. Observation only; nothing here steers the loop. */
      if (rtr.active)
      {
         const char **views = calloc((size_t)call_count * 2, sizeof *views);
         if (views)
         {
            const char **rtr_names = views, **rtr_args = views + call_count;
            for (int i = 0; i < call_count; i++)
            {
               rtr_names[i] = aimee_ir_response_tool(&response, i)->tool_name;
               rtr_args[i] = tool_arguments[i];
            }
            if (rtr_observe_turn(&rtr, rtr_names, rtr_args, call_count))
               aimee_log(LOG_INFO, "agent",
                         "rounds_to_resume=%d rederived_calls=%d basis_sigs=%d basis_dropped=%d",
                         rtr.rounds_to_resume, rtr.rederived_calls, rtr.sig_count,
                         rtr.sigs_dropped);
            free(views);
         }
      }

      for (int i = 0; i < call_count; i++)
      {
         char cancel_reason[128];
         if (agent_durable_cancelled(cancel_reason, sizeof(cancel_reason)))
         {
            snprintf(out->error, sizeof(out->error), "%s before tool %d/%d at turn %d",
                     cancel_reason, i + 1, call_count, turn + 1);
            break;
         }

         total_calls++;

         /* Validate the exact advertised canonical function contract. */
         char val_err[256] = {0};
         const aimee_block_t *tool = aimee_ir_response_tool(&response, i);
         const aimee_tool_t *advertised =
             aimee_ir_find_function(&dispatch_catalog, tool->tool_namespace, tool->tool_name);
         if (!advertised)
            snprintf(val_err, sizeof val_err, "tool identity was not advertised for this request");
         else if (tool->tool_namespace && tool->tool_namespace[0])
            snprintf(val_err, sizeof val_err, "function namespace has no local execution binding");
         if (val_err[0] ||
             tool_validate_ir(advertised, tool_arguments[i], val_err, sizeof(val_err)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               continue;
            }
            snprintf(err_result, 512, "error: validation failed: %s", val_err);
            consecutive_errors++;
            /* Send error back to LLM so it can self-correct */
            if (delegate_append_tool_result(messages, aimee_ir_response_tool(&response, i)->tool_id,
                                            err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof out->error, "cannot retain tool result IR");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s",
                     aimee_ir_response_tool(&response, i)->tool_name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            if (aimee_ir_session_emit_tool_result(aimee_ir_response_tool(&response, i)->tool_id,
                                                  err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof(out->error), "canonical tool result persistence failed");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            free(err_result);
            continue;
         }

         /* Check hard directives */
         char directive_reason[256] = {0};
         if (directive_check_tool(aimee_ir_response_tool(&response, i)->tool_name,
                                  tool_arguments[i], directive_reason,
                                  sizeof(directive_reason)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               continue;
            }
            snprintf(err_result, 512, "error: blocked by directive: %s", directive_reason);
            consecutive_errors++;
            if (delegate_append_tool_result(messages, aimee_ir_response_tool(&response, i)->tool_id,
                                            err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof out->error, "cannot retain tool result IR");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s",
                     aimee_ir_response_tool(&response, i)->tool_name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            if (aimee_ir_session_emit_tool_result(aimee_ir_response_tool(&response, i)->tool_id,
                                                  err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof(out->error), "canonical tool result persistence failed");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            free(err_result);
            continue;
         }

         /* Provider tool IDs need only be unique within one response. Bind
          * accounting to this host-observed turn too, so another turn cannot
          * reuse an old admission while a retry of this dispatch stays stable. */
         char exploration_attempt[96];
         snprintf(exploration_attempt, sizeof(exploration_attempt), "%d:%d", api_call_count, i);
         /* Check policy */
         char policy_reason[256] = {0};
         const char *tool_name = aimee_ir_response_tool(&response, i)->tool_name;
         aimee_tool_metadata_t metadata;
         char effect[32];
         if (tool_policy_metadata(tool_name, &metadata, effect, sizeof effect, policy_reason,
                                  sizeof policy_reason) != 0 ||
             policy_check_tool_metadata_attempt(tool_name, &metadata, effect, tool_arguments[i],
                                                exploration_attempt, policy_reason,
                                                sizeof policy_reason) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               continue;
            }
            snprintf(err_result, 512, "error: blocked by policy: %s", policy_reason);
            consecutive_errors++;
            if (delegate_append_tool_result(messages, aimee_ir_response_tool(&response, i)->tool_id,
                                            err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof out->error, "cannot retain tool result IR");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            snprintf(last_tool_name, sizeof(last_tool_name), "%s",
                     aimee_ir_response_tool(&response, i)->tool_name);
            snprintf(last_tool_result, sizeof(last_tool_result), "%.500s", err_result);
            if (aimee_ir_session_emit_tool_result(aimee_ir_response_tool(&response, i)->tool_id,
                                                  err_result, 1) != 0)
            {
               free(err_result);
               delegate_response_release(&response, response_text, tool_arguments, call_count);
               snprintf(out->error, sizeof(out->error), "canonical tool result persistence failed");
               delegate_capture_stop(out, &capture_failed);
               goto ir_capture_failed;
            }
            free(err_result);
            continue;
         }

         {
            int dj = agent_get_durable_job_id();
            if (dj > 0)
               db1_agent_job_heartbeat_ext(dj, aimee_ir_response_tool(&response, i)->tool_name,
                                           api_call_count);
         }
         agent_tools_set_effect_authorized(1);
         int tool_result_error = 1;
         char *result_str = dispatch_tool_call_ir_ctx_status(advertised, tool_arguments[i],
                                                             agent->timeout_ms, &tool_result_error);
         agent_tools_set_effect_authorized(0);
         result_str = policy_annotate_indexed(aimee_ir_response_tool(&response, i)->tool_name,
                                              tool_arguments[i], exploration_attempt, result_str);
         snprintf(last_tool_name, sizeof last_tool_name, "%s",
                  aimee_ir_response_tool(&response, i)->tool_name);
         snprintf(last_tool_result, sizeof last_tool_result, "%.500s",
                  result_str ? result_str : "");
         if (aimee_ir_session_emit_tool_result(aimee_ir_response_tool(&response, i)->tool_id,
                                               result_str, tool_result_error) != 0)
         {
            free(result_str);
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            snprintf(out->error, sizeof(out->error), "canonical tool result persistence failed");
            delegate_capture_stop(out, &capture_failed);
            goto ir_capture_failed;
         }
         result_str = agent_economize_fresh_tool_result(result_str);
         {
            int dj = agent_get_durable_job_id();
            if (dj > 0)
               db1_agent_job_heartbeat_ext(dj, "", api_call_count);
         }

         /* Execution and receipt owners supply the outcome. A refusal or an
          * unconfirmed receipt is never evidence, regardless of its text. */
         int usable_tool_result = !tool_result_error;
         if (usable_tool_result)
            successful_tool_calls++;

         if (progress_enabled)
         {
            char target[LIVENESS_PROGRESS_TARGET_MAX];
            int range_start = 0, range_end = 0, has_range = 0;
            agent_progress_retrieval(aimee_ir_response_tool(&response, i)->tool_name,
                                     tool_arguments[i], target, sizeof(target), &range_start,
                                     &range_end, &has_range);
            liveness_progress_action_t observed = liveness_progress_observe(
                &progress, aimee_ir_response_tool(&response, i)->tool_name, target, range_start,
                range_end, has_range, usable_tool_result,
                agent_progress_tool_is_mutation(aimee_ir_response_tool(&response, i)->tool_name));
            if (observed > progress_action)
               progress_action = observed;
         }

         /* Track consecutive tool errors for mw_stall_detect */
         if (tool_result_error)
            consecutive_errors++;
         else
            consecutive_errors = 0;

         /* Log trace */
         agent_trace_log(0, turn, "tool_call", NULL,
                         aimee_ir_response_tool(&response, i)->tool_name, tool_arguments[i],
                         result_str, NULL);
         snprintf(last_tool_name, sizeof(last_tool_name), "%s",
                  aimee_ir_response_tool(&response, i)->tool_name);
         snprintf(last_tool_result, sizeof(last_tool_result), "%.500s",
                  result_str ? result_str : "");

         if (delegate_append_tool_result(messages, aimee_ir_response_tool(&response, i)->tool_id,
                                         result_str, tool_result_error) != 0)
         {
            free(result_str);
            delegate_response_release(&response, response_text, tool_arguments, call_count);
            snprintf(out->error, sizeof out->error, "cannot retain tool result IR");
            delegate_capture_stop(out, &capture_failed);
            goto ir_capture_failed;
         }

         free(result_str);
      }

      if (out->error[0] && strstr(out->error, "delegate cancelled"))
      {
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         break;
      }

      if (!out->error[0])
         policy_complete_exploration_turn(turn);

      if (progress_action == LIVENESS_PROGRESS_ABORT)
      {
         snprintf(out->error, sizeof(out->error),
                  "no-progress circuit breaker tripped after %d successful calls without an "
                  "edit (%d repeated or overlapping retrievals)",
                  progress.calls_since_mutation, progress.duplicate_hits);
         delegate_response_release(&response, response_text, tool_arguments, call_count);
         break;
      }
      if (progress_action == LIVENESS_PROGRESS_CHECKPOINT ||
          progress_action == LIVENESS_PROGRESS_ESCALATE)
      {
         char progress_hint[768];
         liveness_format_progress_hint(&progress, progress_action, progress_hint,
                                       sizeof(progress_hint));
         cJSON *hint_ir = agent_session_text_message("user", progress_hint);
         if (hint_ir)
            cJSON_AddItemToArray(messages, hint_ir);
         aimee_log(LOG_WARN, "agent.progress",
                   "checkpoint=%d calls_without_edit=%d duplicate_retrievals=%d",
                   progress.checkpoints, progress.calls_since_mutation, progress.duplicate_hits);
         if (refresh_interval > 2)
            refresh_interval = 2;
      }

      /* Stuck detection: if the agent calls the same tool with the same args
       * LIVENESS_REPEAT_WARN_THRESHOLD times in a row, inject a warning.
       * Circuit breaker trips after LIVENESS_REPEAT_ABORT_THRESHOLD warnings. */
      if (call_count > 0)
      {
         char tool_sig[256];
         snprintf(tool_sig, sizeof(tool_sig), "%s:%.*s",
                  aimee_ir_response_tool(&response, 0)->tool_name, 200,
                  tool_arguments[0] ? tool_arguments[0] : "");
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
                           total_repeat_triggers, aimee_ir_response_tool(&response, 0)->tool_name);
                  delegate_response_release(&response, response_text, tool_arguments, call_count);
                  break;
               }
               /* Inject a hint to the LLM that it appears stuck */
               cJSON *hint_ir = agent_session_text_message(
                   "user", "Warning: You appear to be repeating the same operation. "
                           "Try a different approach or summarize your findings.");
               if (hint_ir)
                  cJSON_AddItemToArray(messages, hint_ir);
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

      delegate_response_release(&response, response_text, tool_arguments, call_count);
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

ir_capture_failed:
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
   out->successful_tool_calls = successful_tool_calls;

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

   if (out->success && canonical_out)
   {
      canonical_out->usage_in = canonical_usage[0];
      canonical_out->usage_out = canonical_usage[1];
      canonical_out->usage_cache_write = canonical_usage[2];
      canonical_out->usage_cache_read = canonical_usage[3];
      canonical_out->usage_reasoning = canonical_usage[4];
   }
   if (out->success && updated_messages)
      *updated_messages = cJSON_Duplicate(messages, 1);

   /* Cleanup */
   aimee_request_free(&dispatch_catalog);
   agent_tools_set_dispatch_role(NULL);
   cJSON_Delete(tools);
   cJSON_Delete(messages);
   free(assembled_sys);
   free(task_instructions);
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

   return context_refused  ? AGENT_RC_CONTEXT_REFUSED
          : capture_failed ? AGENT_RC_CAPTURE_FAILED
                           : (out->success ? 0 : -1);
}

#include "support/delegate_role_seam_stub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern char test_vault_server_codex_oauth[4096];
void test_oauth_tokens_reset(void);
#include "aimee.h"
#include "db1_client/db1.h"
#include "support/store_module_fixture.h"
#include "db1_client/execution_trace.h"
#include "agent.h"
#include "agent_config.h"
#include "runtime_secret.h"
#include <aimee/delegates/delegate_role.h>
#include <aimee/tools/agent_tools.h>
#include "anchor_snapshot.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "modules/workspace/workspace_provider.h"
#include "agent_adapter.h"
#include "provider_cli_adapter.h"
#include "agent_protocol.h"
#include "agent_shell.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include <aimee/audit/audit_action.h>
#include <aimee/audit/audit_worm.h>

void test_agent_route_with_caps_honors_tools_enabled(void);
void test_agent_route_with_caps_honors_context_override(void);
void test_tools_enabled_capability_default(void);
void test_agent_default_primary_skips_disabled(void);
void test_catalog_provider_separates_vendor_from_wire(void);
void test_catalog_provider_explicit_round_trip(void);
void test_unknown_context_window_does_not_pass_min_context(void);
void test_context_window_table_covers_live_vendors(void);
void test_catalog_provider_host_matching_is_label_anchored(void);
void test_catalog_provider_namespaced_model_ids(void);
void test_moonshot_heuristic_scopes_reasoning_to_known_families(void);
void test_catalog_provider_maps_cli_provider_names(void);
void test_primary_turn_reaches_default_above_min_tier(void);
void test_primary_turn_default_must_still_satisfy_caps(void);
void test_catalog_provider_endpoint_parser_edges(void);
void test_request_max_tokens_clamped_to_context_window(void);
void test_registration_prefix(void);
void test_registration_grouping(void);
void test_declared_roles_route_precisely(void);
void test_scope_ceiling_matches_work_to_capability(void);
void test_escalation_target_selection(void);
void test_prefer_local_orders_but_never_bypasses(void);
void test_prefer_healthy_over_degraded(void);
void test_provider_general_registration_expands(void);
void test_provider_general_preserves_explicit_catalog_provider(void);
void test_provider_general_overflow_rejects_config(void);
void test_provider_general_auto_uses_curated_allowlist(void);
void test_provider_general_auto_requires_curated_set(void);
void test_provider_general_rejects_malformed_registrations(void);
void test_capability_routing_flag_behaviour_diff(void);
void test_capability_gate_escalates_instead_of_failing(void);
void test_no_escalation_when_capability_routing_disabled(void);
void test_escalation_respects_policy_and_health_gates(void);

/* Defined in test_agent_responses.c (split out to keep this file under the
 * 2000-line hard limit); called from main() below. */
void test_responses_parser_keeps_all_output_text_parts(void);
void test_responses_parser_accumulates_output_text_deltas(void);
void test_responses_object_folds_in_delta_text(void);
void test_responses_object_folds_in_streamed_function_call(void);
void test_responses_object_keeps_existing_function_call(void);
void test_responses_object_keeps_existing_text(void);
void test_ir_parse_responses_tool_call(void);
void test_ir_parse_responses_namespaced_tool_call(void);
void test_ir_parse_responses_text_only(void);
void test_responses_parser_uses_output_text_done(void);
void test_responses_parser_separates_message_items(void);

/* Strong override of the weak delegation_active_id() (agent_tools_dispatch.c) so a
 * test can simulate running inside a delegation. NULL => the trusted primary
 * session. server_compute_mailbox.o (the real strong definition) is not linked
 * into unit-test-agent, so this override is unambiguous. */
static const char *g_test_delegation_id;
const char *delegation_active_id(void)
{
   return g_test_delegation_id;
}

/* --- Expose tool functions for testing via redeclaration --- */
char *tool_bash(const char *command, int timeout_ms);
char *tool_read_file(const char *path, int offset, int limit, int raw);
char *tool_write_file(const char *path, const char *content);
char *tool_edit_file(const char *path, const char *old_string, const char *new_string,
                     int replace_all);
char *tool_list_files(const char *path, const char *pattern);
char *tool_grep(const char *path, const char *pattern, int max_results);
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms);
int agent_execute_cli_shell_driver(const agent_t *agent, const char *driver_name,
                                   const char *display_name, const char *default_cli_cmd,
                                   const char *system_prompt, const char *user_prompt,
                                   agent_result_t *out);
void test_cancelled_durable_job_blocks_tool_dispatch(void);
void test_delegate_bash_cancel_kills_running_tool(void);
void test_parent_write_guard_readonly_pipeline(void);
void test_stale_parent_guard_blocks_other_worktree_then_clear_unblocks(void);
void test_parent_write_guard_readonly_large_find(void);
void test_parent_write_guard_allows_mkdir_in_delegate_worktree(void);
void test_parent_write_guard_allows_workspace_file_ops(void);
void test_parent_write_guard_allows_workspace_chain(void);
void test_parent_write_guard_allows_readonly_printf(void);
void test_detached_dead_channel_reports_clear_error(void);
void test_container_bash_runs_in_sandbox(void);
void test_container_execute_script_runs_in_sandbox(void);
void otel_init(const char *endpoint, const char *service_name, const char *session)
{
   (void)endpoint;
   (void)service_name;
   (void)session;
}
void otel_on_trace(const char *direction, const char *tool_name, const char *tool_args,
                   const char *tool_result, int turn)
{
   (void)direction;
   (void)tool_name;
   (void)tool_args;
   (void)tool_result;
   (void)turn;
}
const agent_shell_driver_t *agent_shell_driver_get(const char *name)
{
   (void)name;
   return NULL;
}
static cJSON *parse_json_or_die(const char *text)
{
   cJSON *json = cJSON_Parse(text);
   assert(json != NULL);
   return json;
}
static int tools_array_has_name(cJSON *tools, const char *expected)
{
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (!cJSON_IsString(name))
      {
         cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
         name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
      }
      if (cJSON_IsString(name) && strcmp(name->valuestring, expected) == 0)
         return 1;
   }
   return 0;
}

static void test_agent_expand_env(void)
{
   char dst[128];
   assert(runtime_secret_store("AIMEE_TEST_ENV", "expanded") == 0);
   agent_expand_env("$AIMEE_TEST_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "expanded") == 0);
   agent_expand_env("$AIMEE_NO_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "") == 0);
   agent_expand_env("", dst, sizeof(dst));
   assert(strcmp(dst, "") == 0);
   agent_expand_env("plain string", dst, sizeof(dst));
   assert(strcmp(dst, "plain string") == 0);
   runtime_secret_remove("AIMEE_TEST_ENV");
}

static void test_agent_save_never_serializes_literal_key(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "%s", "vault-only");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "%s", "openai");
   snprintf(cfg.agents[0].api_key, sizeof(cfg.agents[0].api_key), "%s", "literal-must-not-land");
   assert(agent_save_config(&cfg) == 0);

   FILE *f = fopen(agent_config_path(), "rb");
   assert(f != NULL);
   char json[4096];
   size_t n = fread(json, 1, sizeof(json) - 1, f);
   fclose(f);
   json[n] = '\0';
   assert(strstr(json, "literal-must-not-land") == NULL);
   assert(strstr(json, "\"api_key\"") == NULL);

   snprintf(cfg.agents[0].api_key, sizeof(cfg.agents[0].api_key), "%s", "$FIRST_BOOT_KEY");
   assert(agent_save_config(&cfg) == 0);
   f = fopen(agent_config_path(), "rb");
   assert(f != NULL);
   n = fread(json, 1, sizeof(json) - 1, f);
   fclose(f);
   json[n] = '\0';
   assert(strstr(json, "$FIRST_BOOT_KEY") != NULL);
   assert(strstr(json, "literal-must-not-land") == NULL);
}

static void test_agent_has_role(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   strcpy(agent.roles[0], "summarize");
   agent.role_count = 1;
   assert(agent_has_role(&agent, "summarize") == 1);
   assert(agent_has_role(&agent, "translate") == 0);
   agent.role_count = 0;
   assert(agent_has_role(&agent, "summarize") == 0);

   /* The "all" wildcard role serves every role. */
   strcpy(agent.roles[0], "all");
   agent.role_count = 1;
   assert(agent_has_role(&agent, "summarize") == 1);
   assert(agent_has_role(&agent, "code") == 1);
   assert(agent_has_role(&agent, "anything") == 1);
}

static void test_agent_supports_persona(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));

   /* No personas listed -> supports every persona (backward compatible). */
   assert(agent_supports_persona(&agent, "architect") == 1);
   assert(agent_supports_persona(&agent, "engineer") == 1);

   /* Explicit list gates by name. */
   strcpy(agent.personas[0], "engineer");
   agent.persona_count = 1;
   assert(agent_supports_persona(&agent, "engineer") == 1);
   assert(agent_supports_persona(&agent, "architect") == 0);

   /* The "all" wildcard serves any persona. */
   strcpy(agent.personas[0], "all");
   agent.persona_count = 1;
   assert(agent_supports_persona(&agent, "engineer") == 1);
   assert(agent_supports_persona(&agent, "architect") == 1);
   assert(agent_supports_persona(&agent, "anything") == 1);

   /* NULL agent -> not supported; NULL/empty persona -> unconstrained. */
   assert(agent_supports_persona(NULL, "engineer") == 0);
   assert(agent_supports_persona(&agent, NULL) == 1);
   assert(agent_supports_persona(&agent, "") == 1);
}

static void test_agent_find(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "agent_one");
   strcpy(cfg.agents[1].name, "agent_two");

   assert(agent_find(&cfg, "agent_one") == &cfg.agents[0]);
   assert(agent_find(&cfg, "agent_two") == &cfg.agents[1]);
   assert(agent_find(&cfg, "missing") == NULL);
}

/* Route-time delegate-policy filter (the server registers a live-config
 * predicate; here a test double): an excluded agent is never routed, even when
 * it is the preferred default agent — the primary must not delegate to itself. */
static int test_policy_exclude_primary(const agent_t *ag)
{
   return strcmp(ag->name, "primary") == 0;
}

static void test_agent_route_policy_filter(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "primary");
   strcpy(cfg.agents[0].name, "primary");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[1].name, "peer");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;

   /* The default is a primary-session preference, not an unpinned delegate pin. */
   agent_routing_set_primary_turn(1);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);
   agent_routing_set_primary_turn(0);

   /* With the policy filter registered, the excluded agent is unroutable
    * EVERYWHERE — including as the preferred default — and routing falls back
    * to the peer. */
   agent_set_route_policy_filter(test_policy_exclude_primary);
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   agent_set_route_policy_filter(NULL);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]); /* sole cheapest peer */
}

/* The server's live predicate consults agent_routing_primary_turn() so the
 * PRIMARY chat turn can route the provider-named agent even though it is
 * excluded as a delegation target (mirrored here by a marker-aware double). */
static int test_policy_exclude_primary_unless_primary_turn(const agent_t *ag)
{
   if (agent_routing_primary_turn())
      return 0;
   return strcmp(ag->name, "primary") == 0;
}

static void test_agent_route_primary_turn_marker(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   strcpy(cfg.default_agent, "primary");
   strcpy(cfg.agents[0].name, "primary");
   strcpy(cfg.agents[0].roles[0], "code");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;

   agent_set_route_policy_filter(test_policy_exclude_primary_unless_primary_turn);
   /* Delegation context: the primary is unroutable and routing fails. */
   assert(agent_route(&cfg, "code") == NULL);
   /* Primary chat turn: the same agent routes for its own turn. */
   agent_routing_set_primary_turn(1);
   assert(agent_route(&cfg, "code") == &cfg.agents[0]);
   agent_routing_set_primary_turn(0);
   assert(agent_route(&cfg, "code") == NULL);
   agent_set_route_policy_filter(NULL);
}

/* Structural rule (no filter needed): a claude-CLI agent that is not
 * server-hosted has no server session to drive — it can never be a delegate,
 * so routing refuses it before any PATH/credential probing. */
static void test_agent_route_client_only_claude_excluded(void)
{
   agent_t a;
   memset(&a, 0, sizeof(a));
   strcpy(a.name, "claude");
   strcpy(a.cli_kind, "claude");
   strcpy(a.backend, AGENT_BACKEND_TMUX_CLI);
   a.enabled = 1;
   a.is_server_hosted = 0;
   assert(agent_is_available_for_routing(&a) == 0);
}

/* agent_routing_block_reason reports WHY an agent is not routable so the delegate
 * error path can name the real cause instead of a catch-all "unavailable". */
static void test_agent_routing_block_reason(void)
{
   char detail[128];

   /* Client-only claude: structural block, no filter needed. */
   agent_t claude;
   memset(&claude, 0, sizeof(claude));
   strcpy(claude.name, "claude");
   strcpy(claude.cli_kind, "claude");
   strcpy(claude.backend, AGENT_BACKEND_TMUX_CLI);
   claude.enabled = 1;
   claude.is_server_hosted = 0;
   assert(agent_routing_block_reason(&claude, detail, sizeof(detail)) ==
          AGENT_ROUTE_CLIENT_ONLY_CLAUDE);

   /* Policy exclusion of a Primary-Agent-Only agent: reason + a naming detail. */
   agent_t primary;
   memset(&primary, 0, sizeof(primary));
   strcpy(primary.name, "primary");
   strcpy(primary.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(primary.cli_cmd, "sh"); /* on PATH, so only the policy filter can block */
   primary.enabled = 1;
   primary.primary_only = 1;
   agent_set_route_policy_filter(test_policy_exclude_primary);
   assert(agent_routing_block_reason(&primary, detail, sizeof(detail)) ==
          AGENT_ROUTE_POLICY_EXCLUDED);
   assert(strstr(detail, "Primary Agent Only") != NULL);
   agent_set_route_policy_filter(NULL);

   /* Missing provider-cli command: reason names the absent binary. */
   agent_t missing;
   memset(&missing, 0, sizeof(missing));
   strcpy(missing.name, "gone");
   strcpy(missing.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(missing.cli_cmd, "definitely-not-a-real-binary-xyz");
   missing.enabled = 1;
   assert(agent_routing_block_reason(&missing, detail, sizeof(detail)) ==
          AGENT_ROUTE_MISSING_COMMAND);
   assert(strstr(detail, "definitely-not-a-real-binary-xyz") != NULL);

   /* A provider-cli agent whose command IS on PATH is routable. */
   agent_t ok;
   memset(&ok, 0, sizeof(ok));
   strcpy(ok.name, "ok");
   strcpy(ok.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(ok.cli_cmd, "sh");
   ok.enabled = 1;
   assert(agent_routing_block_reason(&ok, detail, sizeof(detail)) == AGENT_ROUTE_OK);
}

static void test_agent_route(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;

   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);

   cfg.agents[0].enabled = 0;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.default_agent, "cheap");
   strcpy(cfg.agents[0].name, "cheap");
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   /* Selection is declared-role only: a role must appear in `roles` (or `all`).
    * exec_roles govern tool exposure, not who is routed, so a role-less agent is
    * routable for nothing and a role reaches only the agents that declare it. */
   strcpy(cfg.agents[0].roles[0], "execute");
   cfg.agents[0].role_count = 1;
   strcpy(cfg.agents[1].roles[0], "custom_exec");
   cfg.agents[1].role_count = 1;
   assert(agent_route(&cfg, "execute") == &cfg.agents[0]);
   assert(agent_route(&cfg, "custom_exec") == &cfg.agents[1]);
   assert(agent_route(&cfg, "no_role") == NULL);
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "expensive");
   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 2;
   cfg.agents[1].enabled = 1;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);
   /* A delegate preference is independent from the primary default and may
    * intentionally choose a dearer local/default worker. */
   strcpy(cfg.default_delegate, "expensive");
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   cfg.agents[1].enabled = 0;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);
   cfg.agents[1].enabled = 1;
   cfg.default_delegate[0] = '\0';
   cfg.agents[1].cost_tier = 0;
   /* Equal eligible peers are both used despite an explicit primary default. */
   agent_t *first = agent_route(&cfg, "summarize");
   agent_t *second = agent_route(&cfg, "summarize");
   assert(first != NULL && second != NULL && first != second);
   agent_routing_set_primary_turn(1);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   agent_routing_set_primary_turn(0);
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "missing-cli");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(cfg.agents[0].cli_cmd, "aimee-definitely-missing-provider-cli-test");
   strcpy(cfg.agents[1].name, "http-fallback");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
   assert(agent_is_available_for_routing(&cfg.agents[1]) == 1);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);
   assert(agent_route_at_tier(&cfg, "summarize", 0) == NULL);
   assert(agent_route_at_tier(&cfg, "summarize", 1) == &cfg.agents[1]);
   strcpy(cfg.agents[0].cli_cmd, "/bin/echo ; /bin/true");
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
}

static int g_route_selector_fail;
static int g_route_selector_randomized;
static uint32_t g_route_selector_count;
static uint32_t g_route_selector_pick;

static int test_route_selector(int randomized, uint32_t candidate_count, uint32_t *selected_index)
{
   g_route_selector_randomized = randomized;
   g_route_selector_count = candidate_count;
   if (g_route_selector_fail)
      return -1;
   *selected_index = g_route_selector_pick;
   return 0;
}

static void test_route_authority_refuses_without_provider(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   for (int i = 0; i < 2; i++)
   {
      snprintf(cfg.agents[i].name, sizeof(cfg.agents[i].name), "peer%d", i);
      strcpy(cfg.agents[i].roles[0], "review");
      cfg.agents[i].role_count = 1;
      cfg.agents[i].enabled = 1;
      cfg.agents[i].cost_tier = 0;
   }
   /* No authority yet: the built-in balancer serves equal peers. */
   agent_reset_route_selection_authority();
   assert(agent_route(&cfg, "review") != NULL);
   /* Declare an authority, then lose the provider. The daemon must refuse
    * rather than quietly resume the built-in balancer under a different
    * policy than the operator deployed. */
   agent_set_route_selection_provider(test_route_selector);
   agent_set_route_selection_provider(NULL);
   assert(agent_route(&cfg, "review") == NULL);
   /* The refusal must be legible as a routing fault, not as an empty roster:
    * that distinction is the whole reason the flag exists. */
   assert(agent_route_last_was_module_fault());
   /* Dropping the authority drops the fault with it: a later empty roster must
    * not inherit this outage and be reported as one. */
   agent_reset_route_selection_authority();
   assert(!agent_route_last_was_module_fault());
   assert(agent_route(&cfg, "review") != NULL);
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) != -1);
   /* The role picker is the second seam with the same shape. Exercise it from a
    * cleared flag so this asserts the picker's own bookkeeping and not a fault
    * left behind by agent_route above -- a delegate that routes through here
    * would otherwise still be told the roster was empty. */
   agent_set_route_selection_provider(test_route_selector);
   agent_set_route_selection_provider(NULL);
   assert(!agent_route_last_was_module_fault());
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) == -1);
   assert(agent_route_last_was_module_fault());
   /* The words an operator actually reads. Every caller renders a failed route
    * through this one helper, so this is where the two cases are pinned: each
    * site that spelled the message itself got the distinction wrong. */
   char why[256];
   agent_route_failure_message("review", why, sizeof(why));
   assert(strstr(why, "routing module unavailable") != NULL);
   assert(strstr(why, "no agent available") == NULL);
   agent_reset_route_selection_authority();
   agent_route_failure_message("review", why, sizeof(why));
   assert(strstr(why, "no agent available for role") != NULL);
   assert(strstr(why, "routing module unavailable") == NULL);

   /* A seat that is produced WITHOUT consulting the provider still clears the
    * latch. The single-candidate shortcut returns before the provider is asked,
    * so clearing only on provider success left the fault latched after the
    * module came back -- and the next genuinely empty roster then read as an
    * outage. */
   agent_set_route_selection_provider(test_route_selector);
   agent_set_route_selection_provider(NULL);
   assert(agent_route(&cfg, "review") == NULL);
   assert(agent_route_last_was_module_fault());
   agent_config_t one;
   memset(&one, 0, sizeof(one));
   one.agent_count = 1;
   snprintf(one.agents[0].name, sizeof(one.agents[0].name), "solo");
   strcpy(one.agents[0].roles[0], "review");
   one.agents[0].role_count = 1;
   one.agents[0].enabled = 1;
   one.agents[0].cost_tier = 0;
   /* Deliberately NO reset here: the latch has to still be set going in, or this
    * asserts nothing. One candidate needs no selection, so the shortcut answers
    * before the authority is consulted -- and that success must clear the latch. */
   assert(agent_route_last_was_module_fault());
   assert(agent_route(&one, "review") == &one.agents[0]);
   assert(!agent_route_last_was_module_fault());
   agent_reset_route_selection_authority();
}

static void test_agent_route_selection_provider(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   for (int i = 0; i < cfg.agent_count; ++i)
   {
      snprintf(cfg.agents[i].name, sizeof(cfg.agents[i].name), "worker-%d", i);
      strcpy(cfg.agents[i].roles[0], "review");
      cfg.agents[i].role_count = 1;
      cfg.agents[i].enabled = 1;
      cfg.agents[i].cost_tier = 0;
   }

   agent_set_route_selection_provider(test_route_selector);
   g_route_selector_fail = 0;
   g_route_selector_pick = 1;
   assert(agent_route(&cfg, "review") == &cfg.agents[1]);
   assert(g_route_selector_randomized == 0);
   assert(g_route_selector_count == 2);

   g_route_selector_pick = 0;
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) == 0);
   assert(g_route_selector_randomized == 1);
   assert(g_route_selector_count == 2);

   /* A registered external selector is authoritative: an unavailable process
    * or invalid reply cannot silently resurrect the old in-process decision. */
   g_route_selector_fail = 1;
   assert(agent_route(&cfg, "review") == NULL);
   assert(agent_route_last_was_module_fault());
   /* Clear via a successful pick, so the role-picker assertion below is about
    * that seam rather than the fault agent_route just recorded. */
   g_route_selector_fail = 0;
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) >= 0);
   assert(!agent_route_last_was_module_fault());
   g_route_selector_fail = 1;
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) == -1);
   assert(agent_route_last_was_module_fault());

   g_route_selector_fail = 0;
   g_route_selector_pick = 2;
   assert(agent_route(&cfg, "review") == NULL);
   assert(delegate_pick_for_role(&cfg, "review", NULL, 0) == -1);
   /* Drop the latched authority too: a plain provider=NULL now REFUSES rather
    * than silently resuming the built-in balancer, which is the whole point of
    * the latch. Only a suite may undo it. */
   agent_reset_route_selection_authority();
}

/* The OpenAI Chat and Responses tool surfaces are generated from one builtin
 * tool table (agent_tools.c). This gate locks in the single-source guarantee:
 *   - exact per-surface membership (the surface-specific tools are explicit,
 *     not accidental drift), and
 *   - every tool present on BOTH surfaces has byte-identical name/description/
 *     parameters (modulo the type:function nesting), so the two surfaces can no
 *     longer drift in the content of a shared tool the way they had before. */
static cJSON *tools_find(cJSON *arr, const char *name, int chat)
{
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, arr)
   {
      cJSON *body = chat ? cJSON_GetObjectItem(t, "function") : t;
      cJSON *nm = cJSON_GetObjectItem(body, "name");
      if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0)
         return body;
   }
   return NULL;
}

static int tools_has(cJSON *arr, const char *name, int chat)
{
   return tools_find(arr, name, chat) != NULL;
}

static void test_tool_surface_single_source(void)
{
   cJSON *chat = build_tools_array();
   cJSON *resp = build_tools_array_responses();

   /* Chat tools nest {name,description,parameters} under "function"; Responses
    * tools carry them flat. Both tag type:function. */
   cJSON *first_chat = cJSON_GetArrayItem(chat, 0);
   assert(cJSON_IsObject(cJSON_GetObjectItem(first_chat, "function")));
   cJSON *first_resp = cJSON_GetArrayItem(resp, 0);
   assert(cJSON_IsString(cJSON_GetObjectItem(first_resp, "name")));
   assert(!cJSON_GetObjectItem(first_resp, "function"));

   /* Declared per-surface membership: the surface-specific tools are explicit.
    * code_search (code) and search_docs (docs) are different tools, not a
    * rename — each stays on its surface until a product decision unifies them. */
   assert(tools_has(chat, "code_search", 1) && tools_has(chat, "execute_script", 1));
   assert(!tools_has(chat, "search_docs", 1));
   assert(tools_has(resp, "search_docs", 0));
   assert(!tools_has(resp, "code_search", 0) && !tools_has(resp, "execute_script", 0));

   /* Drift gate: every tool on BOTH surfaces has identical name/description/
    * parameters. cJSON_Compare(case-sensitive) over the normalized body. */
   cJSON *t = NULL;
   int shared = 0;
   cJSON_ArrayForEach(t, chat)
   {
      cJSON *cbody = cJSON_GetObjectItem(t, "function");
      const char *name = cJSON_GetObjectItem(cbody, "name")->valuestring;
      cJSON *rbody = tools_find(resp, name, 0);
      if (!rbody)
         continue; /* surface-specific (execute_script, code_search) */
      shared++;
      assert(cJSON_Compare(cJSON_GetObjectItem(cbody, "description"),
                           cJSON_GetObjectItem(rbody, "description"), 1));
      assert(cJSON_Compare(cJSON_GetObjectItem(cbody, "parameters"),
                           cJSON_GetObjectItem(rbody, "parameters"), 1));
   }
   assert(shared >= 20); /* the bulk of the surface is shared and now consistent */

   cJSON_Delete(chat);
   cJSON_Delete(resp);
}

static void test_knowledge_write_gates_the_tool_policy(void)
{
   /* The carried permission, not the role, is what the tool policy reads. */
   agent_tools_knowledge_write_set(0);
   assert(agent_tools_tool_allowed_for_role("diagnose", "search_memory") == 0);
   agent_tools_knowledge_write_set(1);
   assert(agent_tools_tool_allowed_for_role("validate", "bash") == 1);
   assert(agent_tools_tool_allowed_for_role("validate", "write_file") == 0);
   assert(agent_tools_tool_allowed_for_role("search", "bash") == 0);
   cJSON *tools = build_tools_array();
   assert(tools_array_has_name(tools, "read_file") && tools_array_has_name(tools, "find_symbol"));
   agent_tools_filter_for_role(tools, "review");
   /* review uses review_indexed: the branch-index nav tools plus the read-only
    * worktree tools survive the filter. The read tools are reachability-gated
    * (slice 7); the default provider here is SHARED (reachable), so they are
    * granted. Shell/write tools are never in the toolset. */
   assert(tools_array_has_name(tools, "find_symbol") &&
          tools_array_has_name(tools, "search_memory"));
   assert(tools_array_has_name(tools, "read_file") && !tools_array_has_name(tools, "bash"));
   assert(agent_tools_tool_allowed_for_role("review", "find_symbol") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "search_docs") == 1);
   /* read_file granted under the reachable (SHARED) default; denied only on a
    * DETACHED remote seat — see test_review_read_reachability_gate. */
   assert(agent_tools_tool_allowed_for_role("review", "read_file") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "create_note") == 0);
   /* diagnose stays on the current_code toolset (no index tools). */
   assert(agent_tools_tool_allowed_for_role("diagnose", "find_symbol") == 0);
   cJSON_Delete(tools);
   tools = build_tools_array_anthropic();
   agent_tools_filter_for_role(tools, "diagnose");
   assert(tools_array_has_name(tools, "read_file") && !tools_array_has_name(tools, "find_symbol"));
   cJSON_Delete(tools);
   tools = build_tools_array();
   agent_tools_filter_for_role(tools, "validate");
   assert(tools_array_has_name(tools, "bash") && tools_array_has_name(tools, "read_file"));
   assert(tools_array_has_name(tools, "search_memory") &&
          !tools_array_has_name(tools, "write_file"));
   assert(!tools_array_has_name(tools, "search_docs"));
   cJSON_Delete(tools);
}

/* A delegate that does not hold `shell` cannot run commands, whatever toolset it
 * was given.
 *
 * The independence is the point. A toolset comes from a map keyed on the role
 * NAME, so a role an operator defined without `shell` still resolves to one
 * carrying bash. `code` is used here for exactly that reason: it is a role whose
 * toolset has bash, and the permission is what refuses it.
 */
/* A permission the delegate does not hold withholds its tools, whatever toolset
 * the role resolved to.
 *
 * `code` is the case that mattered: its toolset carries write_file, and a role
 * an operator defined under that name without repo_write used to be handed it
 * anyway. The advertised array and the dispatch check read the SAME list, so
 * what is offered and what is allowed cannot disagree -- both are asserted.
 *
 * WHICH tool needs which permission is the module's list, pinned against the
 * module in server-go/modules/delegates/toolpermissions_test.go. */
/* One delegate's permissions must not become another's.
 *
 * Delegate turns run on pooled worker threads and overlap by design, so these
 * carriers are thread-local. They were not when they were written, which is the
 * bug this pins: a confined delegate would be silently un-confined by whichever
 * concurrent turn wrote last. The same defect was measured once for the active
 * toolset -- three of four turns resolved the last writer's -- and the fix there
 * is the reason this one is stated.
 */
static void *permission_posture_thread(void *unused)
{
   (void)unused;
   /* A second turn, withholding everything. */
   agent_tools_knowledge_write_set(0);
   agent_tools_shell_set(0);
   static const char *const denied[] = {"read_file"};
   agent_tools_denied_set(denied, 1);

   assert(agent_tools_knowledge_write_allowed() == 0);
   assert(agent_tools_shell_allowed() == 0);
   assert(agent_tools_tool_denied("read_file") == 1);
   return NULL;
}

static void test_one_delegates_permissions_are_not_anothers(void)
{
   agent_tools_knowledge_write_set(1);
   agent_tools_shell_set(1);
   agent_tools_denied_set(NULL, 0);

   pthread_t t;
   assert(pthread_create(&t, NULL, permission_posture_thread, NULL) == 0);
   assert(pthread_join(t, NULL) == 0);

   /* This turn is untouched by the other one. */
   assert(agent_tools_knowledge_write_allowed() == 1);
   assert(agent_tools_shell_allowed() == 1);
   assert(agent_tools_tool_denied("read_file") == 0);
   printf("  PASS: test_one_delegates_permissions_are_not_anothers\n");
}

static void test_permissions_clamp_the_toolset(void)
{
   static const char *const denied[] = {"write_file", "edit_file", "git_push"};
   agent_tools_denied_set(denied, 3);

   /* Offered: the tool is gone from the array a `code` delegate is handed. */
   cJSON *tools = build_tools_array();
   assert(tools_array_has_name(tools, "write_file"));
   agent_tools_filter_for_role(tools, "code");
   assert(!tools_array_has_name(tools, "write_file"));
   assert(!tools_array_has_name(tools, "edit_file"));
   /* And what the permission does NOT withhold survives the same filter. */
   assert(tools_array_has_name(tools, "read_file"));
   cJSON_Delete(tools);

   /* Allowed: and the same answer at the gate that runs it. */
   assert(agent_tools_tool_allowed_for_role("code", "write_file") == 0);
   assert(agent_tools_tool_allowed_for_role("code", "read_file") == 1);

   /* Withholding nothing restores it: the clamp is the list, not the role. */
   agent_tools_denied_set(NULL, 0);
   assert(agent_tools_tool_allowed_for_role("code", "write_file") == 1);
   printf("  PASS: test_permissions_clamp_the_toolset\n");
}

static void test_a_delegate_without_shell_cannot_run_commands(void)
{
   agent_tools_set_dispatch_role("code");
   agent_tools_shell_set(0);

   char *result = dispatch_tool_call("bash", "{\"command\":\"ls\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "`shell` permission") != NULL);
   free(result);

   result = dispatch_tool_call("execute_script", "{\"body\":\"echo hi\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "`shell` permission") != NULL);
   free(result);

   /* Holding it, the refusal is gone: whatever bash does next, it is not this. */
   agent_tools_shell_set(1);
   result = dispatch_tool_call("bash", "{\"command\":\"true\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "`shell` permission") == NULL);
   free(result);

   agent_tools_set_dispatch_role(NULL);
   printf("  PASS: test_a_delegate_without_shell_cannot_run_commands\n");
}

static void test_withheld_knowledge_write_blocks_stale_context_tools(void)
{
   agent_tools_set_dispatch_role("diagnose");
   /* Withheld knowledge_write is what closes these doors, and it is carried
    * into the run rather than worked out per call. */
   agent_tools_knowledge_write_set(0);

   char *result = dispatch_tool_call("find_symbol", "{\"identifier\":\"main\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "current-checkout evidence") != NULL);
   free(result);

   result = dispatch_tool_call("aimee:search_memory", "{\"query\":\"prior facts\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "current-checkout evidence") != NULL);
   free(result);

   result = dispatch_tool_call("bash", "{\"command\":\"aimee index scan\"}", 1000);
   assert(result != NULL);
   assert(strstr(result, "mutating or broad aimee context commands are disabled") != NULL);
   free(result);

   agent_tools_knowledge_write_set(1);
   agent_tools_set_dispatch_role(NULL);
}

static void restore_env(const char *name, const char *value)
{
   if (value)
      setenv(name, value, 1);
   else
      unsetenv(name);
}

static void test_provider_env_credentials_and_headers(void)
{
   char old_gemini_mechanism[256] = "";
   const char *v;

   if ((v = getenv("GEMINI_API_KEY_AUTH_MECHANISM")))
      snprintf(old_gemini_mechanism, sizeof(old_gemini_mechanism), "%s", v);

   agent_t ag;
   char auth[512];
   char headers[512];

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "openrouter");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "bearer");
   runtime_secret_remove("OPENROUTER_API_KEY");
   assert(agent_has_resolvable_credentials(&ag) == 0);
   assert(agent_is_available_for_routing(&ag) == 0);
   assert(runtime_secret_store("OPENROUTER_API_KEY", "or-test-key") == 0);
   assert(agent_has_resolvable_credentials(&ag) == 1);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer or-test-key") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "HTTP-Referer: https://github.com/JBailes/aimee") != NULL);
   assert(strstr(headers, "X-Title: aimee") != NULL);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "anthropic");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "api_key");
   assert(runtime_secret_store("ANTHROPIC_API_KEY", "anth-test-key") == 0);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "x-api-key: anth-test-key") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "anthropic-version: 2023-06-01") != NULL);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "gemini");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "api_key");
   runtime_secret_remove("GEMINI_API_KEY");
   unsetenv("GEMINI_API_KEY_AUTH_MECHANISM");
   assert(runtime_secret_store("GOOGLE_API_KEY", "google-test-key") == 0);
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "x-goog-api-key: google-test-key") == 0);

   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "bearer");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer google-test-key") == 0);

   runtime_secret_remove("OPENROUTER_API_KEY");
   runtime_secret_remove("ANTHROPIC_API_KEY");
   runtime_secret_remove("GEMINI_API_KEY");
   runtime_secret_remove("GOOGLE_API_KEY");
   restore_env("GEMINI_API_KEY_AUTH_MECHANISM",
               old_gemini_mechanism[0] ? old_gemini_mechanism : NULL);
}

/* A thin-client-supplied per-turn Codex OAuth token takes precedence over any
 * server-side file, and the account id is injected as the ChatGPT-Account-ID
 * header; clearing removes both. */
static void test_codex_oauth_request_creds(void)
{
   agent_t ag;
   char auth[512];
   char headers[512];
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.provider, sizeof(ag.provider), "%s", "codex");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "codex-oauth");

   agent_set_request_codex_creds("REQ-TOKEN", "acct-1");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer REQ-TOKEN") == 0);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "ChatGPT-Account-ID: acct-1") != NULL);
   assert(strstr(headers, "originator: codex_cli_rs") != NULL);

   /* Clearing the per-turn creds removes the token path + header injection. */
   agent_set_request_codex_creds(NULL, NULL);
   agent_build_extra_headers(&ag, headers, sizeof(headers));
   assert(strstr(headers, "ChatGPT-Account-ID:") == NULL);
}

/* Server-side Codex auth is Vault-only. A legacy auth.json that survives outside
 * the boot migration must never be used as a runtime fallback. */
static void test_codex_oauth_reads_vault_only(void)
{
   const char *old_home = getenv("HOME");
   char saved_home[600] = "";
   if (old_home)
      snprintf(saved_home, sizeof(saved_home), "%s", old_home);
   char dir[256];
   snprintf(dir, sizeof dir, "%s/aimee-codex-home.XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);
   char sub[512], authpath[600];
   snprintf(sub, sizeof(sub), "%s/.codex", dir);
   assert(mkdir(sub, 0700) == 0);
   snprintf(authpath, sizeof(authpath), "%s/auth.json", sub);
   FILE *f = fopen(authpath, "wb");
   assert(f != NULL);
   fputs("{\"tokens\":{\"access_token\":\"DISK-MUST-NOT-WIN\",\"refresh_token\":\"DISK-REFRESH\"}}",
         f);
   fclose(f);
   setenv("HOME", dir, 1);

   agent_t ag;
   char auth[512];
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "%s", "codex");
   snprintf(ag.provider, sizeof(ag.provider), "%s", "codex");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "%s", "codex-oauth");

   test_oauth_tokens_reset();
   snprintf(test_vault_server_codex_oauth, sizeof(test_vault_server_codex_oauth),
            "{\"tokens\":{\"access_token\":\"VAULT-ACCESS-3f9c1\",\"refresh_token\":\"VAULT-"
            "REFRESH\"}}");
   auth[0] = '\0';
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strstr(auth, "Bearer VAULT-ACCESS-3f9c1") != NULL);
   assert(strstr(auth, "DISK-MUST-NOT-WIN") == NULL);

   /* With Vault cleared, even an otherwise valid HOME credential is ignored. */
   test_oauth_tokens_reset();
   test_vault_server_codex_oauth[0] = '\0';
   auth[0] = '\0';
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) != 0);
   if (old_home)
      setenv("HOME", saved_home, 1);
   else
      unsetenv("HOME");
   unlink(authpath);
   rmdir(sub);
   rmdir(dir);
   printf("  PASS: test_codex_oauth_reads_vault_only\n");
}

/* WP-C.2c(3): the vault principal must ride along in the creds snapshot so a
 * fan-out delegate (fresh thread; rebinds via agent_request_creds_restore)
 * reaches the user's vault like a same-thread one; empty restores to empty. */
static void test_request_creds_snapshot_carries_vault_principal(void)
{
   agent_set_request_vault_principal("webuser:dave");
   agent_request_creds_t snap;
   agent_request_creds_snapshot(&snap);
   assert(strcmp(snap.vault_principal, "webuser:dave") == 0);
   agent_set_request_vault_principal(NULL); /* fresh fan-out worker starts clear */
   agent_request_creds_restore(&snap);
   assert(strcmp(agent_get_request_vault_principal(), "webuser:dave") == 0);
   agent_set_request_vault_principal(NULL);
   agent_request_creds_snapshot(&snap);
   agent_set_request_vault_principal("webuser:eve");
   agent_request_creds_restore(&snap);
   assert(agent_get_request_vault_principal()[0] == '\0');
   agent_set_request_session(NULL); /* restore re-bound session+codex; clear all */
   agent_set_request_codex_creds(NULL, NULL);
   agent_set_request_vault_principal(NULL);
}

static void test_agent_config_provider_cli_roundtrip(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   char fake_bin[MAX_PATH_LEN];
   snprintf(fake_bin, sizeof(fake_bin), "%s/aimee-test-agent-bin-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(fake_bin) != NULL);
   char fake_tmux[MAX_PATH_LEN];
   snprintf(fake_tmux, sizeof(fake_tmux), "%s/tmux", fake_bin);
   FILE *tmux = fopen(fake_tmux, "w");
   assert(tmux != NULL);
   fputs("#!/bin/sh\nexit 0\n", tmux);
   fclose(tmux);
   assert(chmod(fake_tmux, 0700) == 0);

   const char *old_path_env = getenv("PATH");
   char *old_path = old_path_env ? strdup(old_path_env) : NULL;
   size_t path_len = strlen(fake_bin) + 2 + (old_path ? strlen(old_path) : 0);
   char *new_path = malloc(path_len);
   assert(new_path != NULL);
   snprintf(new_path, path_len, "%s:%s", fake_bin, old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   free(new_path);

   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"default_delegate\":\"codex-cli\",\"agents\":[{\"name\":\"codex-cli\",\"roles\":["
            "\"code\"],"
            "\"backend\":\"cli-stdio\",\"cli_kind\":\"codex\",\"cli_cmd\":\"codex\","
            "\"cli_idle_timeout_ms\":1234,\"session_reuse\":true},"
            "{\"name\":\"claude\",\"provider\":\"claude\",\"roles\":[\"code\"],"
            "\"backend\":\"provider-cli\",\"cli_kind\":\"claude\",\"cli_cmd\":\"claude-p\"},"
            "{\"name\":\"mistral-plan\",\"provider\":\"mistral\","
            "\"roles\":[\"code\",\"review\",\"explain\",\"refactor\",\"draft\",\"execute\"],"
            "\"backend\":\"provider-cli\",\"cli_kind\":\"mistral-plan\",\"cli_cmd\":\"vibe\","
            "\"cost_tier\":2},"
            "{\"name\":\"gemini-cli\",\"provider\":\"gemini\","
            "\"roles\":[\"code\"],\"backend\":\"provider-cli\","
            "\"cli_kind\":\"gemini\",\"cli_cmd\":\"gemini\",\"enabled\":0},"
            "{\"name\":\"mistral-cli\",\"provider\":\"mistral\","
            "\"roles\":[\"code\"],\"backend\":\"provider-cli\","
            "\"cli_kind\":\"mistral\",\"cli_cmd\":\"mistral\",\"enabled\":1},"
            "{\"name\":\"claude-code\",\"provider\":\"claude-code\","
            "\"roles\":[\"code\"],\"backend\":\"tmux-cli\","
            "\"cli_kind\":\"claude-code\",\"cli_cmd\":\"/bin/echo\","
            "\"session_reuse\":true}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   assert(loaded.agent_count == 6);
   assert(strcmp(loaded.default_delegate, "codex-cli") == 0);
   assert(strcmp(loaded.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(loaded.agents[0].cli_kind, "codex") == 0);
   assert(strcmp(loaded.agents[0].cli_cmd, "codex") == 0);
   assert(loaded.agents[0].cli_idle_timeout_ms == 1234);
   assert(loaded.agents[0].session_reuse == 1);
   assert(strcmp(loaded.agents[1].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(loaded.agents[1].provider, "claude") == 0);
   assert(strcmp(loaded.agents[1].auth_type, "none") == 0);
   assert(loaded.agents[1].cli_kind[0] == '\0');
   assert(strcmp(loaded.agents[1].cli_cmd, "claude") == 0);
   assert(strcmp(loaded.agents[2].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(loaded.agents[2].provider, "mistral") == 0);
   assert(strcmp(loaded.agents[2].cli_kind, "mistral-plan") == 0);
   assert(strcmp(loaded.agents[2].cli_cmd, "vibe") == 0);
   assert(loaded.agents[2].cost_tier == 0);
   assert(strcmp(loaded.agents[2].roles[0], "code") == 0);
   assert(loaded.agents[2].exec_role_count == 0);
   assert(agent_is_exec_role(&loaded.agents[2], "code") == 1);
   assert(strcmp(loaded.agents[3].name, "gemini-cli") == 0);
   assert(strcmp(loaded.agents[3].provider, "gemini") == 0);
   assert(strcmp(loaded.agents[3].cli_kind, "gemini") == 0);
   assert(strcmp(loaded.agents[3].cli_cmd, "gemini") == 0);
   /* Numeric "enabled" (hand-edited rosters) must be honored, not treated as
    * absent-and-therefore-enabled: 0 disables, 1 enables. */
   assert(loaded.agents[3].enabled == 0);
   assert(loaded.agents[4].enabled == 1);
   assert(strcmp(loaded.agents[4].name, "mistral-cli") == 0);
   assert(strcmp(loaded.agents[4].provider, "mistral") == 0);
   assert(strcmp(loaded.agents[4].cli_kind, "mistral") == 0);
   assert(strcmp(loaded.agents[4].cli_cmd, "mistral") == 0);
   assert(strcmp(loaded.agents[5].name, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].provider, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(loaded.agents[5].cli_kind, "claude-code") == 0);
   assert(strcmp(loaded.agents[5].cli_cmd, "/bin/echo") == 0);
   assert(loaded.agents[5].session_reuse == 1);
   /* A claude-CLI agent that is NOT server-hosted (no `is_server_hosted` in its
    * record) is structurally excluded from delegate routing: a client-only
    * claude has no server session to drive, so dispatch could only fail. This
    * short-circuits before any tmux/PATH probing, so it holds everywhere. */
   assert(agent_is_available_for_routing(&loaded.agents[5]) == 0);

   assert(agent_save_config(&loaded) == 0);

   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   assert(reloaded.agent_count == 6);
   assert(strcmp(reloaded.default_delegate, "codex-cli") == 0);
   assert(strcmp(reloaded.agents[0].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(reloaded.agents[0].cli_kind, "codex") == 0);
   assert(strcmp(reloaded.agents[0].cli_cmd, "codex") == 0);
   assert(reloaded.agents[0].cli_idle_timeout_ms == 1234);
   assert(reloaded.agents[0].session_reuse == 1);
   assert(strcmp(reloaded.agents[1].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(reloaded.agents[1].provider, "claude") == 0);
   assert(strcmp(reloaded.agents[1].auth_type, "none") == 0);
   assert(reloaded.agents[1].cli_kind[0] == '\0');
   assert(strcmp(reloaded.agents[1].cli_cmd, "claude") == 0);
   assert(strcmp(reloaded.agents[2].backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(reloaded.agents[2].provider, "mistral") == 0);
   assert(strcmp(reloaded.agents[2].cli_kind, "mistral-plan") == 0);
   assert(strcmp(reloaded.agents[2].cli_cmd, "vibe") == 0);
   assert(reloaded.agents[2].cost_tier == 0);
   assert(strcmp(reloaded.agents[2].roles[0], "code") == 0);
   assert(reloaded.agents[2].exec_role_count == 0);
   assert(agent_is_exec_role(&reloaded.agents[2], "code") == 1);
   assert(strcmp(reloaded.agents[3].name, "gemini-cli") == 0);
   assert(strcmp(reloaded.agents[3].provider, "gemini") == 0);
   assert(strcmp(reloaded.agents[3].cli_kind, "gemini") == 0);
   assert(strcmp(reloaded.agents[3].cli_cmd, "gemini") == 0);
   assert(strcmp(reloaded.agents[4].name, "mistral-cli") == 0);
   assert(strcmp(reloaded.agents[4].provider, "mistral") == 0);
   assert(strcmp(reloaded.agents[4].cli_kind, "mistral") == 0);
   assert(strcmp(reloaded.agents[4].cli_cmd, "mistral") == 0);
   assert(strcmp(reloaded.agents[5].name, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].provider, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].backend, AGENT_BACKEND_TMUX_CLI) == 0);
   assert(strcmp(reloaded.agents[5].cli_kind, "claude-code") == 0);
   assert(strcmp(reloaded.agents[5].cli_cmd, "/bin/echo") == 0);
   assert(reloaded.agents[5].session_reuse == 1);
   /* Still excluded after the save/reload roundtrip (see the load-side assert). */
   assert(agent_is_available_for_routing(&reloaded.agents[5]) == 0);

   if (old_path)
   {
      setenv("PATH", old_path, 1);
      free(old_path);
   }
   else
   {
      unsetenv("PATH");
   }
}

static void test_agent_adapter_registry(void)
{
   const agent_adapter_t *codex = agent_adapter_for_name("codex");
   assert(codex != NULL);
   assert(strcmp(codex->provider, "chatgpt") == 0);
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_DELEGATE_TURN));
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION));
   assert(agent_adapter_supports(codex, AGENT_ADAPTER_CAP_DIRECT_HTTP));

   /* Regression guard (codex-oauth was registered as a tmux-CLI agent instead of
    * HTTP): the OAuth-CLI register path shapes the codex agent from THIS adapter,
    * so its HTTP fields must stay populated — otherwise the registered agent has
    * no reachable endpoint and degrades to the tmux/CLI backend, which then masks
    * local HTTP synth delegates at the same tier (see
    * test_local_synth_not_masked_by_tmux_codex). */
   assert(codex->default_endpoint && codex->default_endpoint[0]);
   assert(strstr(codex->default_endpoint, "codex") != NULL);
   assert(codex->default_model && codex->default_model[0]);
   assert(codex->auth_type && strcmp(codex->auth_type, "codex-oauth") == 0);

   const agent_adapter_t *mistral = agent_adapter_for_provider("mistral");
   assert(mistral != NULL);
   assert(strcmp(mistral->name, "mistral") == 0);
   assert(agent_adapter_supports(mistral, AGENT_ADAPTER_CAP_PRIMARY_SESSION));

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "codex");
   snprintf(ag.provider, sizeof(ag.provider), "chatgpt");
   assert(agent_adapter_for_agent(&ag) == codex);
   assert(agent_adapter_agent_is_direct(&ag) == 1);

   snprintf(ag.backend, sizeof(ag.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   assert(agent_adapter_agent_is_direct(&ag) == 0);
}

/* Regression guard for the codex-tmux misregistration that hid the local synth
 * delegate. agent_route() prefers a tmux-CLI backend over HTTP peers at the
 * cheapest tier (the "has_tmux" filter, mirrored in agent_route_with_caps_inner).
 * When codex was wrongly filed as a tmux-CLI agent at cost_tier 0, that filter
 * excluded the local HTTP synth delegate (gpu-mid) from every default route, so
 * it was never used. Case A reproduces the mask; Case B proves that codex in its
 * correct HTTP (chatgpt) shape leaves the local synth routable. */
static void test_local_synth_not_masked_by_tmux_codex(void)
{
   /* Fake `tmux` and `codex` on PATH so a codex tmux-CLI agent is genuinely
    * available_for_routing (tmux availability requires both binaries on PATH). */
   char fake_bin[MAX_PATH_LEN];
   snprintf(fake_bin, sizeof(fake_bin), "%s/aimee-test-route-bin-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(fake_bin) != NULL);
   const char *fakes[] = {"tmux", "codex"};
   for (size_t i = 0; i < sizeof(fakes) / sizeof(fakes[0]); i++)
   {
      char p[MAX_PATH_LEN];
      snprintf(p, sizeof(p), "%s/%s", fake_bin, fakes[i]);
      FILE *f = fopen(p, "w");
      assert(f != NULL);
      fputs("#!/bin/sh\nexit 0\n", f);
      fclose(f);
      assert(chmod(p, 0700) == 0);
   }
   const char *old_path_env = getenv("PATH");
   char *old_path = old_path_env ? strdup(old_path_env) : NULL;
   size_t path_len = strlen(fake_bin) + 2 + (old_path ? strlen(old_path) : 0);
   char *new_path = malloc(path_len);
   assert(new_path != NULL);
   snprintf(new_path, path_len, "%s:%s", fake_bin, old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   free(new_path);

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.default_agent, sizeof(cfg.default_agent), "gpu-mid");

   /* Local HTTP synth delegate at the cheapest tier. Empty provider => keyless =>
    * available_for_routing, matching a no-auth local endpoint. */
   agent_t *synth = &cfg.agents[0];
   snprintf(synth->name, sizeof(synth->name), "gpu-mid");
   snprintf(synth->roles[0], sizeof(synth->roles[0]), "execute");
   synth->role_count = 1;
   synth->cost_tier = 0;
   synth->enabled = 1;
   synth->tools_enabled = 1;

   /* Peer codex at the same cheapest tier. */
   agent_t *codex = &cfg.agents[1];
   snprintf(codex->name, sizeof(codex->name), "codex");
   snprintf(codex->roles[0], sizeof(codex->roles[0]), "execute");
   codex->role_count = 1;
   codex->cost_tier = 0;
   codex->enabled = 1;
   codex->tools_enabled = 1;

   /* Case A — the bug: codex mis-filed as tmux-CLI. has_tmux fires at tier 0 and
    * excludes the HTTP synth entirely, so the router can only pick the tmux peer. */
   snprintf(codex->backend, sizeof(codex->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(codex->cli_kind, sizeof(codex->cli_kind), "codex");
   snprintf(codex->cli_cmd, sizeof(codex->cli_cmd), "codex");
   assert(agent_is_available_for_routing(synth) == 1);
   assert(agent_is_available_for_routing(codex) == 1); /* tmux + codex on PATH */
   agent_t *routed_a = agent_route(&cfg, "execute");
   assert(routed_a == codex); /* synth masked by the tmux peer */
   assert(routed_a != synth);

   /* Case B — the fix: codex in its correct HTTP (chatgpt) shape. No tmux backend
    * at the cheapest tier, so the local synth is ROUTABLE again.
    *
    * Both peers now sit at tier 0, and agent_pick_balanced() round-robins them
    * on a PROCESS-WIDE static cursor. Asserting a single call returns synth was
    * therefore an assertion about cursor parity, not about masking — it passed
    * only because of how many routing calls happened to run before it, and any
    * new test elsewhere in the binary could flip it. The property that actually
    * matters is that synth is reachable at all, so sample the rotation. */
   codex->backend[0] = '\0';
   codex->cli_kind[0] = '\0';
   codex->cli_cmd[0] = '\0';
   snprintf(codex->provider, sizeof(codex->provider), "chatgpt");
   snprintf(codex->auth_type, sizeof(codex->auth_type), "codex-oauth");
   int saw_synth = 0, saw_codex = 0;
   for (int i = 0; i < 8; i++)
   {
      agent_t *r = agent_route(&cfg, "execute");
      if (r == synth)
         saw_synth = 1;
      else if (r == codex)
         saw_codex = 1;
      else
         assert(0 && "routed to an unexpected agent");
   }
   assert(saw_synth); /* the masking bug would make this impossible */
   assert(saw_codex); /* and both peers share the tier, so both must appear */

   if (old_path)
   {
      setenv("PATH", old_path, 1);
      free(old_path);
   }
   else
      unsetenv("PATH");
   printf("  PASS: test_local_synth_not_masked_by_tmux_codex\n");
}

static void test_provider_cli_shell_exec_uses_argv_not_shell(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "cat-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/cat");
   ag.cli_idle_timeout_ms = 5000;

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Cat CLI", NULL, "system", "user",
                                         &result) == 0);
   assert(result.success == 1);
   assert(result.response != NULL);
   assert(strcmp(result.response, "system\n\nuser") == 0);
   free(result.response);

   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "bad-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/cat ; /bin/echo injected");
   ag.cli_idle_timeout_ms = 5000;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Bad CLI", NULL, NULL, "user", &result) !=
          0);
   assert(strstr(result.error, "shell operators") != NULL);
}

static void test_provider_cli_shell_timeout_covers_prompt_write(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "sleep-cli");
   snprintf(ag.cli_cmd, sizeof(ag.cli_cmd), "/bin/sleep 2");
   ag.cli_idle_timeout_ms = 100;

   char *large_prompt = malloc(256 * 1024);
   assert(large_prompt != NULL);
   memset(large_prompt, 'x', (256 * 1024) - 1);
   large_prompt[(256 * 1024) - 1] = '\0';

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   assert(agent_execute_cli_shell_driver(&ag, "missing", "Sleep CLI", NULL, NULL, large_prompt,
                                         &result) != 0);
   assert(strstr(result.error, "timed out") != NULL);
   free(large_prompt);
}

static void test_codex_oauth_auth_resolution(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.auth_type, sizeof(ag.auth_type), "codex-oauth");

   char auth[MAX_API_KEY_LEN + 32];
   test_oauth_tokens_reset();
   snprintf(test_vault_server_codex_oauth, sizeof(test_vault_server_codex_oauth),
            "{\"access_token\":\"codex-test-token\",\"refresh_token\":\"codex-refresh\"}");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer codex-test-token") == 0);

   test_oauth_tokens_reset();
   snprintf(test_vault_server_codex_oauth, sizeof(test_vault_server_codex_oauth),
            "{\"tokens\":{\"access_token\":\"codex-cli-token\",\"refresh_token\":\"codex-cli-"
            "refresh\"}}");
   assert(agent_resolve_auth(&ag, auth, sizeof(auth)) == 0);
   assert(strcmp(auth, "Authorization: Bearer codex-cli-token") == 0);
   test_oauth_tokens_reset();
   test_vault_server_codex_oauth[0] = '\0';
}

static void test_agent_is_exec_role(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));

   /* No explicit exec_roles: use defaults. CANONICAL names only — `test` and
    * `implement` used to be listed here but are ALIASES (delegate_role.c maps
    * them to validate/code), and every routing path canonicalises before this is
    * reached (cmd_agent_delegate.c, server_compute.c), so they could never match
    * and were dead entries. */
   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 1);
   assert(agent_is_exec_role(&agent, "diagnose") == 1);
   assert(agent_is_exec_role(&agent, "execute") == 1);
   assert(agent_is_exec_role(&agent, "code") == 1);
   assert(agent_is_exec_role(&agent, "refactor") == 1);
   assert(agent_is_exec_role(&agent, "draft") == 1);
   /* Aliases are NOT exec roles; they resolve to their canonical form first. */
   assert(agent_is_exec_role(&agent, "test") == 0);
   assert(agent_is_exec_role(&agent, "implement") == 0);
   assert(strcmp(delegate_role_canonicalize("test"), "validate") == 0);
   assert(strcmp(delegate_role_canonicalize("implement"), "code") == 0);
   assert(agent_is_exec_role(&agent, "summarize") == 0);
   /* Novel-mode checks the novel persona genuinely delegates stay. */
   assert(agent_is_exec_role(&agent, "continuity") == 1);
   assert(agent_is_exec_role(&agent, "beat-check") == 1);
   /* Songwriter/novel WRITE work was culled: no persona could reach it. */
   assert(agent_is_exec_role(&agent, "lyric") == 0);
   assert(agent_is_exec_role(&agent, "prosody") == 0);
   assert(agent_is_exec_role(&agent, "prose") == 0);
   assert(agent_is_exec_role(&agent, "line-edit") == 0);
   assert(agent_is_exec_role(&agent, "hook") == 0);
   assert(agent_is_exec_role(&agent, "songform") == 0);

   /* With explicit exec_roles */
   strcpy(agent.exec_roles[0], "deploy");
   strcpy(agent.exec_roles[1], "custom_role");
   agent.exec_role_count = 2;

   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "custom_role") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 0);
   assert(agent_is_exec_role(&agent, "execute") == 0);
}

static void test_tool_bash(void)
{
   /* Basic echo */
   char *result = tool_bash("echo hello", 5000);
   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *out = cJSON_GetObjectItem(json, "stdout");
   assert(out && cJSON_IsString(out));
   assert(strstr(out->valuestring, "hello") != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Non-zero exit code */
   result = tool_bash("exit 42", 5000);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 42);
   cJSON_Delete(json);
   free(result);

   /* Timeout */
   result = tool_bash("sleep 60", 200);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == -1);
   cJSON_Delete(json);
   free(result);

   result = tool_bash("yes x | head -c 65536", 5000);
   assert(result && strstr(result, "\"exit_code\":0") != NULL);
   free(result);
}

/* Containment: a DELEGATE (untrusted model) must never run a shell UNSANDBOXED on
 * the aimee-server host — that host is uid 0 with the docker socket mounted, so an
 * unsandboxed command is a host-root escalation. With a delegation active tool_bash
 * must refuse instead of forking on the host. The primary session (no delegation)
 * still runs.
 *
 * The sandbox is OPTED OUT explicitly here rather than relying on the config default:
 * the default is now SANDBOX_MODE_WORKSPACE_ONLY, so an inherited default no longer
 * reaches the unsandboxed branch at all, and a test that depends on it would silently
 * stop exercising the containment it exists to prove. `sandbox: {mode: off}` is also
 * the realistic shape of this risk — an operator who turned isolation off. */
/* Containment: a DELEGATE (untrusted model) must never run a shell UNSANDBOXED on the
 * aimee-server host — that host is uid 0 with the docker socket mounted, so an
 * unsandboxed command is a host-root escalation. With a delegation active tool_bash
 * must refuse rather than fork on the host; the primary session still runs.
 *
 * The mode is forced through the test seam rather than through config. The mode now
 * defaults to WORKSPACE_ONLY, and this binary cannot redirect config in-process (the
 * config path resolves before a test can move HOME), so without the override this
 * fail-closed branch would silently stop being exercised. */
static void test_tool_bash_delegate_unsandboxed_refused(void)
{
   sandbox_set_mode_override_for_test(SANDBOX_MODE_OFF);

   g_test_delegation_id = "test-deleg";
   char *result = tool_bash("echo escalated", 5000);
   assert(result != NULL);
   assert(strstr(result, "refused") != NULL);   /* fail-closed */
   assert(strstr(result, "escalated") == NULL); /* the command did NOT run */
   assert(strstr(result, "\"exit_code\":-1") != NULL);
   /* Names the setting rather than blaming an "unavailable" sandbox. */
   assert(strstr(result, "sandbox.mode=off") != NULL);
   free(result);

   /* The trusted primary (operator) session still runs on the host. */
   g_test_delegation_id = NULL;
   result = tool_bash("echo primary-ok", 5000);
   assert(result != NULL);
   assert(strstr(result, "primary-ok") != NULL);
   free(result);

   /* With the sandbox ON, the same delegated command is contained by ISOLATION
    * instead of refusal — it runs, rather than being refused. This is the branch the
    * new default puts real installs on, so it is asserted rather than assumed. */
   sandbox_set_mode_override_for_test(SANDBOX_MODE_WORKSPACE_ONLY);
   g_test_delegation_id = "test-deleg";
   result = tool_bash("echo contained", 5000);
   assert(result != NULL);
   assert(strstr(result, "refused") == NULL);
   assert(strstr(result, "\"exit_code\":0") != NULL);
   free(result);

   g_test_delegation_id = NULL;
   sandbox_set_mode_override_for_test(-1); /* restore production behaviour */
}

/* A write into a source checkout is redirected into an aimee-managed worktree
 * by guardrails (pre_tool_check returns 1 with the rewritten path) under the
 * default (shared) provider. When a `detached` workspace provider is active the
 * tool marshals to the serving client (file tools + tool_bash -> exec_shell), so
 * the server-side worktree rewrite must be SKIPPED — otherwise the path/command
 * would be re-pointed at a checkout that does not exist on the client. This
 * locks in the guardrails skip that lets detached delegates operate on the
 * client's live tree. */
static void test_detached_skips_worktree_rewrite(void)
{
   char repo[256];
   snprintf(repo, sizeof(repo), "%s/det_wt_test.XXXXXX", platform_tmpdir());
   assert(mkdtemp(repo) != NULL);
   char shellcmd[512];
   snprintf(shellcmd, sizeof(shellcmd), "git init -q '%s' >/dev/null 2>&1", repo);
   (void)system(shellcmd);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char input[512];
   snprintf(input, sizeof(input), "{\"file_path\":\"%s/foo.c\",\"content\":\"int x;\"}", repo);
   char msg[1024];

   /* Shared (default) provider: the write is redirected into a worktree. */
   workspace_provider_clear_active();
   msg[0] = '\0';
   int rc_shared = pre_tool_check("Write", input, &state, MODE_APPROVE, repo, msg, sizeof(msg));
   assert(rc_shared == 1);                /* allow-with-rewrite */
   assert(strstr(msg, ".aimee") != NULL); /* rewritten into an aimee worktree */

   /* Detached provider active: the rewrite is skipped (path left untouched). */
   workspace_provider_t fake_detached;
   memset(&fake_detached, 0, sizeof(fake_detached));
   fake_detached.kind = WS_PROVIDER_DETACHED;
   workspace_provider_set_active(&fake_detached);
   msg[0] = '\0';
   int rc_detached = pre_tool_check("Write", input, &state, MODE_APPROVE, repo, msg, sizeof(msg));
   workspace_provider_clear_active();
   assert(rc_detached != 1);              /* allow-with-rewrite did NOT fire */
   assert(strstr(msg, ".aimee") == NULL); /* no worktree path was injected */
   /* NB: rc_detached here reflects the orthogonal, config-based write gate
    * (cwd_is_detached_workspace) which this test's bare temp repo is not
    * registered for. In production workspace_turn_bind_active and
    * cwd_is_detached_workspace read the SAME workspace registry, so a real
    * detached turn lifts both and the write is allowed (rc 0). What this test
    * pins is the one thing the provider bind controls: the worktree rewrite. */

   snprintf(shellcmd, sizeof(shellcmd), "rm -rf '%s'", repo);
   (void)system(shellcmd);

   printf("  detached_skips_worktree_rewrite: ok (shared=%d, detached=%d)\n", rc_shared,
          rc_detached);
}

/* The guardrail redirects a shell command into the session worktree by returning
 * 3 with the rewritten line ("cd <worktree> && <cmd>") — the command-shaped twin
 * of the rc==1 path rewrite. cmd_hooks.c applies both; the delegate tool dispatch
 * applied only rc==1, so a rewrite arrived as "not 0" and was reported as a
 * refusal, with the rewritten command as the reason. Every shell command a
 * delegate ran came back "guardrail blocked: cd <worktree> && <cmd>" — `pwd` and
 * `echo hello` included — so a delegate could edit files and never run one
 * command. This pins the verdict and that it is applied, not refused. */
static void test_shell_worktree_rewrite_is_applied_not_refused(void)
{
   char repo[256];
   snprintf(repo, sizeof(repo), "%s/shell_wt_rw.XXXXXX", platform_tmpdir());
   assert(mkdtemp(repo) != NULL);
   char shellcmd[512];
   snprintf(shellcmd, sizeof(shellcmd), "git init -q '%s' >/dev/null 2>&1", repo);
   (void)system(shellcmd);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   workspace_provider_clear_active();
   char msg[1024] = "";
   int rc = pre_tool_check("bash", "{\"command\":\"pwd\"}", &state, MODE_APPROVE, repo, msg,
                           sizeof(msg));

   if (rc == 3)
   {
      /* The verdict carries a rewritten command, not a refusal reason. */
      assert(strncmp(msg, "cd ", 3) == 0);
      assert(strstr(msg, ".aimee") != NULL);
      assert(strstr(msg, "pwd") != NULL);

      /* End to end: the dispatch must APPLY that rewrite, not report it. Create
       * the worktree the guardrail wants to redirect into, then run the same
       * tool call through the real dispatch and require both that it was not
       * refused and that the command actually ran there. Before the fix this
       * returned "error: guardrail blocked: cd ... && pwd". */
      char target[768] = "";
      const char *start = msg + 3; /* past "cd " */
      const char *end = strstr(start, " && ");
      assert(end != NULL && (size_t)(end - start) < sizeof(target));
      memcpy(target, start, (size_t)(end - start));
      target[end - start] = '\0';
      snprintf(shellcmd, sizeof(shellcmd), "mkdir -p '%s'", target);
      assert(system(shellcmd) == 0);

      run_cmd_set_cwd(repo);
      char *out = dispatch_tool_call("bash", "{\"command\":\"pwd\"}", 10000);
      run_cmd_set_cwd(NULL);
      assert(out != NULL);
      if (strstr(out, "guardrail blocked") != NULL)
      {
         fprintf(stderr, "  dispatch refused the rewrite instead of applying it: %s\n", out);
         assert(0 && "shell worktree rewrite was reported as a refusal");
      }
      /* pwd ran inside the redirected worktree, so its output names that path. */
      assert(strstr(out, target) != NULL);
      free(out);
      printf("  shell_worktree_rewrite: rc=3 applied; pwd ran in %.48s...\n", target);
   }
   else
   {
      /* Not every environment reaches the rewrite (it needs a session worktree
       * to redirect into). Say so rather than pass silently on a case that never
       * exercised the contract. */
      printf("  shell_worktree_rewrite: skipped (rc=%d, no worktree to redirect into)\n", rc);
   }

   snprintf(shellcmd, sizeof(shellcmd), "rm -rf '%s'", repo);
   (void)system(shellcmd);
}

static void test_tool_read_file(void)
{
   /* Write a temp file */
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_read_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);
   const char *content = "line1\nline2\nline3\nline4\n";
   if (write(fd, content, strlen(content)) < 0)
   { /* ignore */
   }
   close(fd);

   /* Read entire file */
   char *result = tool_read_file(tmppath, 0, 0, 1);
   assert(result != NULL);
   assert(strcmp(result, content) == 0);
   free(result);

   /* Read with offset */
   result = tool_read_file(tmppath, 1, 0, 1);
   assert(result != NULL);
   assert(strcmp(result, "line2\nline3\nline4\n") == 0);
   free(result);

   /* Read with limit */
   result = tool_read_file(tmppath, 0, 2, 1);
   assert(result != NULL);
   assert(strcmp(result, "line1\nline2\n") == 0);
   free(result);

   /* Nonexistent path */
   result = tool_read_file("/nonexistent/path/file.txt", 0, 0, 1);
   assert(result != NULL);
   assert(strstr(result, "error: parent path cannot be resolved") != NULL);
   free(result);

   unsigned char binary_content[] = {0x7f, 'E', 'L', 'F', '\0', 0x01, 0x02, 0x03};
   assert((fd = open(tmppath, O_WRONLY | O_TRUNC)) >= 0);
   assert(write(fd, binary_content, sizeof(binary_content)) == (ssize_t)sizeof(binary_content));
   close(fd);
   result = tool_read_file(tmppath, 0, 0, 1);
   assert(result != NULL);
   assert(strstr(result, "error: binary file omitted") != NULL);
   assert(memchr(result, 0x7f, strlen(result)) == NULL);
   free(result);

   /* Anchored mode (default): lines prefixed LINE:HASH| and a snapshot header. */
   assert((fd = open(tmppath, O_WRONLY | O_TRUNC)) >= 0);
   assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
   close(fd);
   result = tool_read_file(tmppath, 0, 0, 0);
   assert(result != NULL);
   assert(strstr(result, "snapshot=s") != NULL);
   assert(strstr(result, "| line1") != NULL);
   assert(strstr(result, "| line4") != NULL);
   /* an anchor of the shape "N:HH| " must be present */
   assert(strstr(result, "1:") != NULL);
   free(result);

   unlink(tmppath);
}

static void test_tool_write_file(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_write_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);

   char *result = tool_write_file(tmppath, "hello world");
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *status = cJSON_GetObjectItem(json, "status");
   cJSON *changed = cJSON_GetObjectItem(json, "changed");
   cJSON *summary = cJSON_GetObjectItem(json, "summary");
   cJSON *diff = cJSON_GetObjectItem(json, "diff");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   assert(changed && cJSON_IsTrue(changed));
   assert(summary && strstr(summary->valuestring, "+1") != NULL);
   assert(diff && cJSON_GetObjectItem(diff, "additions")->valueint == 1);
   assert(cJSON_GetObjectItem(diff, "deletions")->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Verify contents */
   char *readback = tool_read_file(tmppath, 0, 0, 1);
   assert(readback != NULL);
   assert(strcmp(readback, "hello world") == 0);
   free(readback);

   /* Overwrite and verify structured diff output */
   result = tool_write_file(tmppath, "hello changed");
   assert(result != NULL);
   json = parse_json_or_die(result);
   status = cJSON_GetObjectItem(json, "status");
   changed = cJSON_GetObjectItem(json, "changed");
   summary = cJSON_GetObjectItem(json, "summary");
   diff = cJSON_GetObjectItem(json, "diff");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   assert(changed && cJSON_IsTrue(changed));
   assert(summary && strstr(summary->valuestring, "+1") != NULL);
   assert(diff && cJSON_GetObjectItem(diff, "additions")->valueint == 1);
   assert(cJSON_GetObjectItem(diff, "deletions")->valueint == 1);
   cJSON_Delete(json);
   free(result);

   /* Write identical content — should return plain "ok" */
   result = tool_write_file(tmppath, "hello changed");
   assert(result != NULL);
   assert(strcmp(result, "ok") == 0);
   free(result);

   /* Text tools must reject malformed UTF-8 without changing the file. */
   char malformed[] = {'b', 'a', 'd', (char)0xc2, '\0'};
   result = tool_write_file(tmppath, malformed);
   assert(result != NULL);
   assert(strstr(result, "not valid UTF-8") != NULL);
   free(result);
   readback = tool_read_file(tmppath, 0, 0, 1);
   assert(readback != NULL);
   assert(strcmp(readback, "hello changed") == 0);
   free(readback);

   unlink(tmppath);
}

static void test_tool_edit_file(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_edit_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);

   char *result = tool_write_file(tmppath, "alpha\nbeta\ngamma\nbeta\n");
   assert(result != NULL);
   free(result);

   /* Unique replacement succeeds and returns a structured diff. */
   result = tool_edit_file(tmppath, "gamma", "GAMMA", 0);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *status = cJSON_GetObjectItem(json, "status");
   assert(status && strcmp(status->valuestring, "ok") == 0);
   cJSON_Delete(json);
   free(result);
   char *readback = tool_read_file(tmppath, 0, 0, 1);
   assert(readback && strcmp(readback, "alpha\nbeta\nGAMMA\nbeta\n") == 0);
   free(readback);

   /* old_string absent → error, file unchanged. */
   result = tool_edit_file(tmppath, "does-not-exist", "x", 0);
   assert(result != NULL && strncmp(result, "error:", 6) == 0);
   free(result);

   /* Non-unique old_string without replace_all → error, file unchanged. */
   result = tool_edit_file(tmppath, "beta", "B", 0);
   assert(result != NULL && strncmp(result, "error:", 6) == 0);
   free(result);
   readback = tool_read_file(tmppath, 0, 0, 1);
   assert(readback && strcmp(readback, "alpha\nbeta\nGAMMA\nbeta\n") == 0);
   free(readback);

   /* replace_all rewrites every occurrence. */
   result = tool_edit_file(tmppath, "beta", "B", 1);
   assert(result != NULL && strncmp(result, "error:", 6) != 0);
   free(result);
   readback = tool_read_file(tmppath, 0, 0, 1);
   assert(readback && strcmp(readback, "alpha\nB\nGAMMA\nB\n") == 0);
   free(readback);

   unlink(tmppath);
}

/* Pull "snapshot=<id>" out of an anchored read_file header line. */
static char *anchored_snapshot_id(const char *read_out, char *buf, size_t buflen)
{
   const char *m = strstr(read_out, "snapshot=");
   if (!m)
      return NULL;
   m += strlen("snapshot=");
   size_t i = 0;
   while (m[i] && m[i] != ' ' && m[i] != '\n' && i + 1 < buflen)
   {
      buf[i] = m[i];
      i++;
   }
   buf[i] = '\0';
   return buf;
}

static cJSON *mk_replace(const char *anchor, const char *text)
{
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "op", "replace");
   cJSON_AddStringToObject(e, "at", anchor);
   cJSON_AddStringToObject(e, "text", text);
   return e;
}

/* Find the "N:HH" anchor for 1-based ordinal `ord` in an anchored read_file
 * output (a line beginning "N:HH| "). */
static char *anchored_line_token(const char *read_out, int ord, char *buf, size_t buflen)
{
   char needle[16];
   snprintf(needle, sizeof(needle), "\n%d:", ord);
   const char *m = strstr(read_out, needle);
   if (!m)
      return NULL;
   m++; /* skip the leading newline */
   size_t i = 0;
   while (m[i] && m[i] != '|' && m[i] != ' ' && i + 1 < buflen)
   {
      buf[i] = m[i];
      i++;
   }
   buf[i] = '\0';
   return buf;
}

static void test_tool_edit_file_anchored(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_test_anchored_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);

   char *w = tool_write_file(tmppath, "one\ntwo\nthree\nfour\n");
   assert(w);
   free(w);

   /* anchored read mints a snapshot */
   char *rd = tool_read_file(tmppath, 0, 0, 0);
   assert(rd);
   char sid[ANCHOR_SNAPSHOT_ID_MAX];
   assert(anchored_snapshot_id(rd, sid, sizeof(sid)) && sid[0] == 's');
   char a2[24], a3[24];
   assert(anchored_line_token(rd, 2, a2, sizeof(a2)));
   assert(anchored_line_token(rd, 3, a3, sizeof(a3)));
   free(rd);

   /* dry_run previews without writing */
   cJSON *dedits = cJSON_CreateArray();
   cJSON_AddItemToArray(dedits, mk_replace(a2, "TWO"));
   char *edits_json = cJSON_PrintUnformatted(dedits);
   (void)edits_json;
   char *dry = tool_edit_file_anchored(tmppath, sid, dedits, 1);
   assert(dry);
   cJSON *dj = parse_json_or_die(dry);
   assert(strcmp(cJSON_GetObjectItem(dj, "status")->valuestring, "dry_run") == 0);
   cJSON_Delete(dj);
   free(dry);
   free(edits_json);
   cJSON_Delete(dedits);
   char *rb = tool_read_file(tmppath, 0, 0, 1);
   assert(rb && strcmp(rb, "one\ntwo\nthree\nfour\n") == 0); /* unchanged by dry_run */
   free(rb);

   /* commit a two-op batch */
   cJSON *edits = cJSON_CreateArray();
   cJSON_AddItemToArray(edits, mk_replace(a2, "TWO"));
   cJSON_AddItemToArray(edits, mk_replace(a3, "THREE"));
   char *res = tool_edit_file_anchored(tmppath, sid, edits, 0);
   assert(res && strncmp(res, "error:", 6) != 0);
   cJSON *rj = parse_json_or_die(res);
   assert(strcmp(cJSON_GetObjectItem(rj, "status")->valuestring, "ok") == 0);
   cJSON_Delete(rj);
   free(res);
   cJSON_Delete(edits);
   rb = tool_read_file(tmppath, 0, 0, 1);
   assert(rb && strcmp(rb, "one\nTWO\nTHREE\nfour\n") == 0);
   free(rb);

   /* the old snapshot is now stale: the file moved under it */
   cJSON *stale_edits = cJSON_CreateArray();
   cJSON_AddItemToArray(stale_edits, mk_replace(a2, "again"));
   char *stale = tool_edit_file_anchored(tmppath, sid, stale_edits, 0);
   assert(stale);
   cJSON *sj = parse_json_or_die(stale);
   assert(strcmp(cJSON_GetObjectItem(sj, "status")->valuestring, "stale_anchor") == 0);
   /* rejection carries a fresh snapshot_id to retry against */
   assert(cJSON_GetObjectItem(sj, "snapshot_id") != NULL);
   cJSON_Delete(sj);
   free(stale);
   cJSON_Delete(stale_edits);

   /* unknown snapshot id -> snapshot_expired, file untouched */
   cJSON *exp_edits = cJSON_CreateArray();
   cJSON_AddItemToArray(exp_edits, mk_replace("2:aa", "z"));
   char *exp = tool_edit_file_anchored(tmppath, "sdeadbeef00000000", exp_edits, 0);
   assert(exp);
   cJSON *ej = parse_json_or_die(exp);
   assert(strcmp(cJSON_GetObjectItem(ej, "status")->valuestring, "snapshot_expired") == 0);
   cJSON_Delete(ej);
   free(exp);
   cJSON_Delete(exp_edits);

   unlink(tmppath);
}

/* A large single-span anchored rewrite must carry the edit_symbol steering
 * advisory (roundtable P5-completion guardrail); a small one must not. */
static void test_anchored_large_span_advisory(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_span_XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   close(fd);
   char *w = tool_write_file(tmppath, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
   assert(w);
   free(w);

   char *rd = tool_read_file(tmppath, 0, 0, 0);
   assert(rd);
   char sid[ANCHOR_SNAPSHOT_ID_MAX];
   assert(anchored_snapshot_id(rd, sid, sizeof(sid)));
   char a2[24], a9[24];
   assert(anchored_line_token(rd, 2, a2, sizeof(a2)));
   assert(anchored_line_token(rd, 9, a9, sizeof(a9)));
   free(rd);

   /* replace_range spanning lines 2..9 (8 lines) -> advisory expected */
   cJSON *edits = cJSON_CreateArray();
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "op", "replace_range");
   cJSON_AddStringToObject(e, "from", a2);
   cJSON_AddStringToObject(e, "to", a9);
   cJSON_AddStringToObject(e, "text", "X");
   cJSON_AddItemToArray(edits, e);
   char *res = tool_edit_file_anchored(tmppath, sid, edits, 0);
   assert(res && strncmp(res, "error:", 6) != 0);
   cJSON *rj = parse_json_or_die(res);
   assert(cJSON_GetObjectItem(rj, "advisory") != NULL);
   assert(strstr(cJSON_GetObjectItem(rj, "advisory")->valuestring, "edit_symbol") != NULL);
   cJSON_Delete(rj);
   free(res);
   cJSON_Delete(edits);

   /* boundary: a 7-line span (2..8) is BELOW the threshold -> no advisory.
    * Restore the 10-line file first (the edit above shrank it). */
   char *wb = tool_write_file(tmppath, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
   assert(wb);
   free(wb);
   rd = tool_read_file(tmppath, 0, 0, 0);
   assert(rd);
   assert(anchored_snapshot_id(rd, sid, sizeof(sid)));
   char b2[24], b8[24];
   assert(anchored_line_token(rd, 2, b2, sizeof(b2)));
   assert(anchored_line_token(rd, 8, b8, sizeof(b8)));
   free(rd);
   cJSON *e7 = cJSON_CreateArray();
   cJSON *r7 = cJSON_CreateObject();
   cJSON_AddStringToObject(r7, "op", "replace_range");
   cJSON_AddStringToObject(r7, "from", b2);
   cJSON_AddStringToObject(r7, "to", b8);
   cJSON_AddStringToObject(r7, "text", "Y");
   cJSON_AddItemToArray(e7, r7);
   char *res7 = tool_edit_file_anchored(tmppath, sid, e7, 0);
   assert(res7 && strncmp(res7, "error:", 6) != 0);
   cJSON *rj7 = parse_json_or_die(res7);
   assert(cJSON_GetObjectItem(rj7, "advisory") == NULL); /* 7 < 8 */
   cJSON_Delete(rj7);
   free(res7);
   cJSON_Delete(e7);

   /* delete_range spanning >= 8 lines also carries the advisory */
   char *w2 = tool_write_file(tmppath, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
   assert(w2);
   free(w2);
   rd = tool_read_file(tmppath, 0, 0, 0);
   assert(rd);
   assert(anchored_snapshot_id(rd, sid, sizeof(sid)));
   char d2[24], d9[24];
   assert(anchored_line_token(rd, 2, d2, sizeof(d2)));
   assert(anchored_line_token(rd, 9, d9, sizeof(d9)));
   free(rd);
   cJSON *ed = cJSON_CreateArray();
   cJSON *rd_op = cJSON_CreateObject();
   cJSON_AddStringToObject(rd_op, "op", "delete_range");
   cJSON_AddStringToObject(rd_op, "from", d2);
   cJSON_AddStringToObject(rd_op, "to", d9);
   cJSON_AddItemToArray(ed, rd_op);
   char *resd = tool_edit_file_anchored(tmppath, sid, ed, 0);
   assert(resd && strncmp(resd, "error:", 6) != 0);
   cJSON *rjd = parse_json_or_die(resd);
   assert(cJSON_GetObjectItem(rjd, "advisory") != NULL);
   cJSON_Delete(rjd);
   free(resd);
   cJSON_Delete(ed);

   /* a small single-line replace must NOT carry the advisory */
   char *w3 = tool_write_file(tmppath, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
   assert(w3);
   free(w3);
   rd = tool_read_file(tmppath, 0, 0, 0);
   assert(rd);
   assert(anchored_snapshot_id(rd, sid, sizeof(sid)));
   char a1[24];
   assert(anchored_line_token(rd, 1, a1, sizeof(a1)));
   free(rd);
   cJSON *e2 = cJSON_CreateArray();
   cJSON_AddItemToArray(e2, mk_replace(a1, "L1"));
   char *res2 = tool_edit_file_anchored(tmppath, sid, e2, 0);
   assert(res2 && strncmp(res2, "error:", 6) != 0);
   cJSON *rj2 = parse_json_or_die(res2);
   assert(cJSON_GetObjectItem(rj2, "advisory") == NULL);
   cJSON_Delete(rj2);
   free(res2);
   cJSON_Delete(e2);

   unlink(tmppath);
}

static void test_part3_anchored_tools(void)
{
   char dir[512];
   snprintf(dir, sizeof(dir), "%s/aimee_p3_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);
   char cfile[600];
   snprintf(cfile, sizeof(cfile), "%s/sample.c", dir);
   const char *src = "#include <stdio.h>\n"
                     "int add(int a, int b)\n"
                     "{\n"
                     "   return a + b;\n"
                     "}\n"
                     "int main(void)\n"
                     "{\n"
                     "   return add(1, 2);\n"
                     "}\n";
   char *w = tool_write_file(cfile, src);
   assert(w);
   free(w);

   /* outline: symbol skeleton with a snapshot + anchors */
   char *outline = tool_read_outline(cfile);
   assert(outline);
   assert(strstr(outline, "snapshot=s") != NULL);
   assert(strstr(outline, "add") != NULL && strstr(outline, "main") != NULL);
   free(outline);

   /* read_symbol: just the def span, anchored + headed */
   char *sym = tool_read_symbol("add", cfile);
   assert(sym);
   assert(strstr(sym, "# add") != NULL);
   assert(strstr(sym, "| int add(int a, int b)") != NULL);
   assert(strstr(sym, "| int main") == NULL); /* only the add span */
   free(sym);

   /* grep anchored: hits become editable anchors under a per-file snapshot */
   char *g = tool_grep_anchored(dir, "return", 50);
   assert(g);
   assert(strstr(g, "snapshot=s") != NULL);
   assert(strstr(g, "|") != NULL);
   free(g);

   /* run_tests: structured pass/fail with counts+failures, full log spilled */
   char *rt = tool_run_tests("printf 'ok\\n'; exit 0", 10000);
   assert(rt);
   cJSON *rj = parse_json_or_die(rt);
   assert(strcmp(cJSON_GetObjectItem(rj, "status")->valuestring, "passed") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(rj, "passed")));
   cJSON_Delete(rj);
   free(rt);

   char *rf = tool_run_tests("echo boom; exit 1", 10000);
   assert(rf);
   cJSON *fj = parse_json_or_die(rf);
   assert(strcmp(cJSON_GetObjectItem(fj, "status")->valuestring, "failed") == 0);
   assert(!cJSON_IsTrue(cJSON_GetObjectItem(fj, "passed")));
   cJSON_Delete(fj);
   free(rf);

   /* edit_symbol: whole-function rewrite by name, span resolved server-side */
   char *es = tool_edit_symbol("add", cfile, "replace_body",
                               "int add(int a, int b)\n{\n   return a + b + 0;\n}");
   assert(es);
   cJSON *ej = parse_json_or_die(es);
   assert(strcmp(cJSON_GetObjectItem(ej, "status")->valuestring, "ok") == 0);
   /* identity echo so the agent can confirm the resolved target */
   assert(cJSON_GetObjectItem(ej, "symbol") != NULL);
   assert(cJSON_GetObjectItem(ej, "resolved_at") != NULL);
   cJSON_Delete(ej);
   free(es);
   char *chk = tool_read_file(cfile, 0, 0, 1);
   assert(chk && strstr(chk, "return a + b + 0;") != NULL);
   assert(strstr(chk, "int main") != NULL); /* main untouched */
   free(chk);

   unlink(cfile);
   rmdir(dir);
}

/* web_read.c exports this SSRF classifier. */
int web_egress_addr_blocked(const struct sockaddr *sa);

static int v4_blocked(const char *ip)
{
   struct sockaddr_in sa = {0};
   sa.sin_family = AF_INET;
   assert(inet_pton(AF_INET, ip, &sa.sin_addr) == 1);
   return web_egress_addr_blocked((struct sockaddr *)&sa);
}

static int v6_blocked(const char *ip)
{
   struct sockaddr_in6 sa = {0};
   sa.sin6_family = AF_INET6;
   assert(inet_pton(AF_INET6, ip, &sa.sin6_addr) == 1);
   return web_egress_addr_blocked((struct sockaddr *)&sa);
}

static void test_web_read_ssrf(void)
{
   /* private / reserved / metadata addresses are blocked */
   assert(v4_blocked("127.0.0.1"));
   assert(v4_blocked("10.1.2.3"));
   assert(v4_blocked("172.16.5.4"));
   assert(v4_blocked("192.168.0.1"));
   assert(v4_blocked("169.254.169.254")); /* cloud metadata */
   assert(v4_blocked("100.64.0.1"));      /* CGNAT */
   assert(v4_blocked("0.0.0.0"));
   assert(v4_blocked("224.0.0.1"));
   /* public addresses are allowed */
   assert(!v4_blocked("8.8.8.8"));
   assert(!v4_blocked("93.184.216.34"));
   /* IPv6 */
   assert(v6_blocked("::1"));
   assert(v6_blocked("fe80::1"));
   assert(v6_blocked("fc00::1"));
   assert(v6_blocked("::ffff:127.0.0.1")); /* mapped loopback */
   assert(!v6_blocked("2001:4860:4860::8888"));

   /* end-to-end: the tool refuses non-http schemes and private targets without
    * any network egress */
   char *r = tool_web_read("ftp://example.com/x", NULL, 0, NULL);
   assert(r && strstr(r, "error:") != NULL);
   free(r);
   r = tool_web_read("http://127.0.0.1:1/", NULL, 0, NULL);
   assert(r && strstr(r, "error:") != NULL);
   free(r);
   r = tool_web_read("http://169.254.169.254/latest/meta-data/", NULL, 0, NULL);
   assert(r && strstr(r, "error:") != NULL);
   free(r);
   r = tool_web_read("not-a-url", NULL, 0, NULL);
   assert(r && strstr(r, "error:") != NULL);
   free(r);
}

static void test_parent_write_guard_blocks_parent_writes(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char src_dir[512];
   snprintf(src_dir, sizeof(src_dir), "%s/src", root);
   assert(platform_mkdir_p(src_dir, 0700) == 0 || access(src_dir, F_OK) == 0);

   char aimee_dir[512];
   char worktrees_dir[512];
   char delegate_dir[512];
   char worktree[512];
   snprintf(aimee_dir, sizeof(aimee_dir), "%s/.aimee", root);
   snprintf(worktrees_dir, sizeof(worktrees_dir), "%s/worktrees", aimee_dir);
   snprintf(delegate_dir, sizeof(delegate_dir), "%s/delegate", worktrees_dir);
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   char parent_file[512];
   snprintf(parent_file, sizeof(parent_file), "%s/src/owned.c", root);
   FILE *f = fopen(parent_file, "w");
   assert(f != NULL);
   fputs("original", f);
   fclose(f);

   char worktree_file[512];
   snprintf(worktree_file, sizeof(worktree_file), "%s/owned.c", worktree);

   char sibling_file[512];
   snprintf(sibling_file, sizeof(sibling_file), "%s-sibling/owned.c", root);

   char root_with_slash[512];
   snprintf(root_with_slash, sizeof(root_with_slash), "%s/", root);
   agent_tools_parent_write_guard_set(root_with_slash, worktree);
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 1);
   assert(agent_tools_parent_write_guard_blocks("src/owned.c", root) == 1);
   assert(agent_tools_parent_write_guard_blocks(worktree_file, NULL) == 0);
   assert(agent_tools_parent_write_guard_blocks(sibling_file, NULL) == 0);

   char *result = tool_write_file(parent_file, "blocked");
   assert(result != NULL);
   assert(strstr(result, "parent worktree is read-only") != NULL);
   free(result);

   result = tool_read_file(parent_file, 0, 0, 1);
   assert(result != NULL);
   assert(strcmp(result, "original") == 0);
   free(result);

   char parent_command_file[512];
   char verify_cmd[1024];
   snprintf(parent_command_file, sizeof(parent_command_file), "%s/src/command-owned.c", root);
   snprintf(verify_cmd, sizeof(verify_cmd), "touch %s", parent_command_file);
   result = tool_verify("command_succeeds", verify_cmd, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *pass = cJSON_GetObjectItem(json, "pass");
   cJSON *reason = cJSON_GetObjectItem(json, "reason");
   assert(pass && cJSON_IsFalse(pass));
   assert(reason && strstr(reason->valuestring, "parent worktree is read-only") != NULL);
   cJSON_Delete(json);
   free(result);
   assert(access(parent_command_file, F_OK) != 0);

   agent_tools_parent_write_guard_set(root_with_slash, root);
   assert(agent_tools_parent_write_guard_root() == NULL);
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 0);
   agent_tools_parent_write_guard_set(root_with_slash, worktree);

   result = tool_write_file(worktree_file, "allowed");
   assert(result != NULL);
   assert(strstr(result, "error:") == NULL);
   free(result);

   result = tool_read_file(worktree_file, 0, 0, 1);
   assert(result != NULL);
   assert(strcmp(result, "allowed") == 0);
   free(result);

   /* Read-only-delegate gate (write_capable): a delegate that is not
    * write-capable is refused ALL writes — even inside its own worktree, where
    * the parent-write guard would otherwise permit them. Mirrors the codex
    * read-only sandbox for the native tool backend. */
   agent_tools_write_capable_set(0);
   assert(agent_tools_readonly_delegate_blocks() == 1);
   result = tool_write_file(worktree_file, "blocked-readonly");
   assert(result != NULL);
   assert(strstr(result, "read-only delegate") != NULL);
   free(result);
   /* Restoring write capability re-permits the same worktree write. */
   agent_tools_write_capable_set(1);
   assert(agent_tools_readonly_delegate_blocks() == 0);
   result = tool_write_file(worktree_file, "allowed");
   assert(result != NULL);
   assert(strstr(result, "error:") == NULL);
   free(result);

   run_cmd_set_cwd(worktree);
   result = tool_bash("pwd", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, worktree) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char worktree_subdir[512];
   snprintf(worktree_subdir, sizeof(worktree_subdir), "%s/subdir", worktree);
   assert(mkdir(worktree_subdir, 0700) == 0);
   run_cmd_set_cwd(worktree);
   char cd_pwd_cmd[1024];
   snprintf(cd_pwd_cmd, sizeof(cd_pwd_cmd), "cd %s && pwd", worktree_subdir);
   result = tool_bash(cd_pwd_cmd, 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, worktree_subdir) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char script_path[512];
   char wrote_path[512];
   snprintf(script_path, sizeof(script_path), "%s/write-ok.sh", worktree);
   snprintf(wrote_path, sizeof(wrote_path), "%s/wrote.txt", worktree);
   f = fopen(script_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\nprintf allowed > wrote.txt\n", f);
   fclose(f);
   assert(chmod(script_path, 0700) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("./write-ok.sh", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   assert(access(wrote_path, F_OK) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("make --version", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char fake_aimee_path[512];
   char old_path[8192] = "";
   int had_old_path = 0;
   snprintf(fake_aimee_path, sizeof(fake_aimee_path), "%s/aimee", worktree);
   f = fopen(fake_aimee_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf index-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = memory ] && [ \"$2\" = search ]; then\n"
         "  printf memory-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = delegate ] && [ \"$2\" = status ]; then\n"
         "  printf delegate-status-ok\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = jobs ] && [ \"$2\" = logs ]; then\n"
         "  printf jobs-logs-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(fake_aimee_path, 0700) == 0);
   const char *path_env = getenv("PATH");
   if (path_env)
   {
      had_old_path = 1;
      snprintf(old_path, sizeof(old_path), "%s", path_env);
   }
   char new_path[8704];
   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   char cd_index_cmd[1024];
   snprintf(cd_index_cmd, sizeof(cd_index_cmd), "cd %s && aimee index overview", worktree);
   result = tool_bash(cd_index_cmd, 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview && aimee memory search delegate", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "index-ok") != NULL);
   assert(strstr(stdout_item->valuestring, "memory-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   snprintf(new_path, sizeof(new_path), "%s:%s", worktree, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee delegate status 123 && aimee jobs logs 123", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "delegate-status-ok") != NULL);
   assert(strstr(stdout_item->valuestring, "jobs-logs-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   const char *home = getenv("HOME");
   assert(home && home[0]);
   char external_bin[512];
   char external_aimee[512];
   snprintf(external_bin, sizeof(external_bin), "%s/.aimee-test-external-bin-XXXXXX", home);
   assert(platform_mkdtemp(external_bin) != NULL);
   snprintf(external_aimee, sizeof(external_aimee), "%s/aimee", external_bin);
   f = fopen(external_aimee, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf external-index-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(external_aimee, 0700) == 0);

   snprintf(new_path, sizeof(new_path), "%s:%s", external_bin, had_old_path ? old_path : "");
   setenv("PATH", new_path, 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, "external-index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(external_bin);

   char saved_home[512] = "";
   int had_home = 0;
   if (home && home[0])
   {
      had_home = 1;
      snprintf(saved_home, sizeof(saved_home), "%s", home);
   }
   char tmp_home[512];
   char tmp_local[512];
   char tmp_local_bin[512];
   char tmp_local_aimee[512];
   snprintf(tmp_home, sizeof(tmp_home), "%s/home", root);
   snprintf(tmp_local, sizeof(tmp_local), "%s/.local", tmp_home);
   snprintf(tmp_local_bin, sizeof(tmp_local_bin), "%s/bin", tmp_local);
   snprintf(tmp_local_aimee, sizeof(tmp_local_aimee), "%s/aimee", tmp_local_bin);
   assert(mkdir(tmp_home, 0700) == 0);
   assert(mkdir(tmp_local, 0700) == 0);
   assert(mkdir(tmp_local_bin, 0700) == 0);
   f = fopen(tmp_local_aimee, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = index ] && [ \"$2\" = overview ]; then\n"
         "  printf local-bin-index-ok\n"
         "  exit 0\n"
         "fi\n"
         "exit 64\n",
         f);
   fclose(f);
   assert(chmod(tmp_local_aimee, 0700) == 0);

   setenv("HOME", tmp_home, 1);
   setenv("PATH", "/usr/bin:/bin", 1);
   run_cmd_set_cwd(worktree);
   result = tool_bash("aimee index overview", 5000);
   run_cmd_set_cwd(NULL);
   if (had_home)
      setenv("HOME", saved_home, 1);
   else
      unsetenv("HOME");
   if (had_old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   if (!strstr(stdout_item->valuestring, "local-bin-index-ok"))
   {
      cJSON *stderr_item = cJSON_GetObjectItem(json, "stderr");
      fprintf(stderr, "local-bin resolution output: stdout=%s stderr=%s exit=%d\n",
              stdout_item->valuestring,
              stderr_item && cJSON_IsString(stderr_item) ? stderr_item->valuestring : "<missing>",
              ec && cJSON_IsNumber(ec) ? ec->valueint : -999);
   }
   assert(strstr(stdout_item->valuestring, "local-bin-index-ok") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   agent_tools_parent_write_guard_clear();
   assert(agent_tools_parent_write_guard_blocks(parent_file, NULL) == 0);

   unlink(tmp_local_aimee);
   unlink(fake_aimee_path);
   unlink(wrote_path);
   unlink(script_path);
   unlink(worktree_file);
   unlink(parent_file);
   rmdir(tmp_local_bin);
   rmdir(tmp_local);
   rmdir(tmp_home);
   rmdir(worktree);
   rmdir(delegate_dir);
   rmdir(worktrees_dir);
   rmdir(aimee_dir);
   rmdir(src_dir);
   rmdir(root);
}

static void test_parent_write_guard_shell_uses_delegate_cwd_in_git_parent(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_git_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char cmd[2048];
   int rc = 0;
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   char *out = run_cmd(cmd, &rc);
   free(out);
   assert(rc == 0);

   char parent_worktree[512];
   char delegate_worktree[512];
   snprintf(parent_worktree, sizeof(parent_worktree), "%s/.aimee/worktrees/parent/main", root);
   snprintf(delegate_worktree, sizeof(delegate_worktree), "%s/.aimee/worktrees/delegate/main",
            root);
   assert(platform_mkdir_p(parent_worktree, 0700) == 0 || access(parent_worktree, F_OK) == 0);
   assert(platform_mkdir_p(delegate_worktree, 0700) == 0 || access(delegate_worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(parent_worktree, delegate_worktree);
   run_cmd_set_cwd(delegate_worktree);

   char *result = tool_bash("pwd", 5000);
   run_cmd_set_cwd(NULL);

   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, delegate_worktree) != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   run_cmd_set_cwd(delegate_worktree);
   result = tool_bash("git rev-parse --show-prefix", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();
   assert(result != NULL);
   json = parse_json_or_die(result);
   stdout_item = cJSON_GetObjectItem(json, "stdout");
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strstr(stdout_item->valuestring, ".aimee/worktrees/delegate/main/") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(root);
}
static void test_git_tools_default_to_session_worktree(void)
{
   /* git_status/git_log/git_diff must default 'path' to the delegate's session
    * worktree (the run_cmd cwd) when omitted. A delegate does not know its own
    * worktree path, so a hard "missing 'path'" error forced it to a raw `git`
    * shell — which the require_aimee_git gate then denies, leaving it no working
    * git route at all. Omitting 'path' must resolve to the cwd, not error. */
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_git_default_cwd_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);
   char cmd[1024];
   int rc = 0;
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   char *out = run_cmd(cmd, &rc);
   free(out);
   if (rc != 0)
   {
      platform_test_rmrf(root);
      printf("  git_tools_default_to_session_worktree: skipped (git unavailable)\n");
      return;
   }

   run_cmd_set_cwd(root);
   char *st = dispatch_tool_call("git_status", "{}", 5000);
   char *lg = dispatch_tool_call("git_log", "{}", 5000);
   char *df = dispatch_tool_call("git_diff", "{}", 5000);
   run_cmd_set_cwd(NULL);

   /* None of the three read-only git tools may error for a missing 'path' now
    * that it defaults to the session worktree. */
   assert(st != NULL && strstr(st, "requires a session worktree") == NULL &&
          strstr(st, "missing 'path'") == NULL);
   assert(lg != NULL && strstr(lg, "requires a session worktree") == NULL &&
          strstr(lg, "missing 'path'") == NULL);
   assert(df != NULL && strstr(df, "requires a session worktree") == NULL &&
          strstr(df, "missing 'path'") == NULL);
   free(st);
   free(lg);
   free(df);
   platform_test_rmrf(root);
   printf("  git_tools_default_to_session_worktree: ok\n");
}
static void test_tool_list_files(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_list_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char alpha[512], beta[512], dockerfile[512], compose[512];
   snprintf(alpha, sizeof(alpha), "%s/alpha.txt", tmpdir);
   snprintf(beta, sizeof(beta), "%s/beta.log", tmpdir);
   snprintf(dockerfile, sizeof(dockerfile), "%s/Dockerfile.test", tmpdir);
   snprintf(compose, sizeof(compose), "%s/compose.yaml", tmpdir);
   const char *fixtures[][2] = {
       {alpha, "alpha"}, {beta, "beta"}, {dockerfile, "from scratch"}, {compose, "services: {}"}};
   for (int i = 0; i < 4; i++)
   {
      FILE *f = fopen(fixtures[i][0], "w");
      assert(f != NULL);
      fputs(fixtures[i][1], f);
      fclose(f);
   }
   char *result = tool_list_files(tmpdir, "*.txt");
   assert(result != NULL);
   assert(strstr(result, "alpha.txt") != NULL);
   assert(strstr(result, "beta.log") == NULL);
   free(result);
   result = tool_list_files(tmpdir, "**/Dockerfile*");
   assert(result != NULL);
   assert(strstr(result, "Dockerfile.test") != NULL);
   assert(strstr(result, "compose.yaml") == NULL);
   free(result);
   result = tool_list_files("/nonexistent_dir_12345", NULL);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);
   unlink(alpha);
   unlink(beta);
   unlink(dockerfile);
   unlink(compose);
   rmdir(tmpdir);
}
static void test_tool_grep_excludes_heavy_dirs(void)
{
   const char *excluded_dirs[] = {".git", ".aimee", "build", "dist", "node_modules"};
   char tmpdir[512], heavy_dirs[5][512], alpha[512], vendored[5][512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_grep_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      snprintf(heavy_dirs[i], sizeof(heavy_dirs[i]), "%s/%s", tmpdir, excluded_dirs[i]);
      assert(mkdir(heavy_dirs[i], 0700) == 0);
   }
   snprintf(alpha, sizeof(alpha), "%s/alpha.txt", tmpdir);

   FILE *f = fopen(alpha, "w");
   assert(f != NULL);
   fputs("needle normal\n", f);
   fclose(f);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      snprintf(vendored[i], sizeof(vendored[i]), "%s/ignored.txt", heavy_dirs[i]);
      f = fopen(vendored[i], "w");
      assert(f != NULL);
      fputs("needle vendored\n", f);
      fclose(f);
   }

   char *result = tool_grep(tmpdir, "needle", 20);
   assert(result != NULL);
   assert(strstr(result, "alpha.txt") != NULL);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
      assert(strstr(result, excluded_dirs[i]) == NULL);
   free(result);

   unlink(alpha);
   for (size_t i = 0; i < sizeof(excluded_dirs) / sizeof(excluded_dirs[0]); i++)
   {
      unlink(vendored[i]);
      rmdir(heavy_dirs[i]);
   }
   rmdir(tmpdir);
}

static void test_dispatch_tool_call(void)
{
   /* bash tool */
   char *result = dispatch_tool_call("bash", "{\"command\":\"echo dispatch_test\"}", 5000);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "dispatch_test") != NULL);
   cJSON_Delete(json);
   free(result);

   /* Unknown tool */
   result = dispatch_tool_call("unknown_tool", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: unknown tool") != NULL);
   free(result);

   /* Unknown tool with suggestion */
   result = dispatch_tool_call("read_flie", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "Did you mean 'read_file'?") != NULL);
   free(result);

   result = dispatch_tool_call("writ_file", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "Did you mean 'write_file'?") != NULL);
   free(result);

   /* Argument alias: "cmd" → "command" */
   result = dispatch_tool_call("bash", "{\"cmd\":\"echo alias_test\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "alias_test") != NULL);
   assert(strstr(result, "error") == NULL);
   free(result);

   /* execute_script is write-capable and therefore requires the managed
    * worktree context that a real delegate turn supplies. */
   char script_root[256];
   snprintf(script_root, sizeof script_root, "%s/aimee-script-dispatch.XXXXXX", platform_tmpdir());
   assert(mkdtemp(script_root) != NULL);
   char script_cwd[MAX_PATH_LEN];
   assert(snprintf(script_cwd, sizeof(script_cwd), "%s/.aimee/worktrees/unit-test-agent/main",
                   script_root) < (int)sizeof(script_cwd));
   assert(platform_mkdir_p(script_cwd, 0700) == 0 || access(script_cwd, F_OK) == 0);
   run_cmd_set_cwd(script_cwd);
   result = dispatch_tool_call(
       "execute_script",
       "{\"language\":\"python\",\"body\":\"print('script_dispatch')\",\"timeout_secs\":5}", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = parse_json_or_die(result);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "script_dispatch") != NULL);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Argument alias: "filepath" → "file_path" then falls through to "path" */
   result = dispatch_tool_call("read_file", "{\"filepath\":\"/dev/null\"}", 5000);
   assert(result != NULL);
   /* Should not fail with "missing 'path'" */
   assert(strstr(result, "missing") == NULL);
   free(result);

   /* Type coercion: string "5" → integer for offset */
   result = dispatch_tool_call("read_file", "{\"path\":\"/dev/null\",\"offset\":\"5\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error") == NULL);
   free(result);

   /* Reversible writes are contracted and independently read back before the
    * dispatcher reports success. */
   run_cmd_set_cwd(script_cwd);
   result = dispatch_tool_call(
       "write_file", "{\"path\":\"contract.txt\",\"content\":\"verified write\\n\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error:") == NULL);
   free(result);
   result = dispatch_tool_call("read_file", "{\"path\":\"contract.txt\",\"raw\":true}", 5000);
   assert(result != NULL && strcmp(result, "verified write\n") == 0);
   free(result);
   result = dispatch_tool_call("write_file", "{\"content\":\"no target\"}", 5000);
   assert(result != NULL && strstr(result, "effect contract refused") != NULL);
   free(result);
   run_cmd_set_cwd(NULL);

   /* Write file error includes recovery hint */
   run_cmd_set_cwd(script_cwd);
   result = dispatch_tool_call("write_file", "{\"path\":\"nonexistent/dir/file\"}", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   if (strstr(result, "error"))
      assert(strstr(result, "Recovery:") != NULL);
   free(result);
   platform_test_rmrf(script_root);

   /* Missing parameter */
   result = dispatch_tool_call("bash", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: missing 'command' parameter") != NULL);
   free(result);
}

#include "agent_source_authority.h"
#include <pthread.h>

/* Regression test for the concurrent-delegate source-authority race: each
 * delegate installs its source context via agent_source_authority_tls_set
 * (thread-local), and the code_search overlay must read THIS thread's context,
 * never a neighbor's. Pre-fix the context lived in process-global env vars, so
 * 8 threads each setting a different worktree/paths clobbered each other and the
 * overlay resolved against the wrong tree (flakiness that varied with timing).
 *
 * Two thread groups (A/B) each own a distinct file whose unique symbol exists
 * ONLY in their file. Each thread, looping, sets its TLS paths and asserts the
 * overlay finds ITS symbol (>=1 hit) and NOT the other group's symbol (0 hits).
 * Any cross-thread TLS leak flips one of those assertions. */
typedef struct
{
   const char *path;      /* this group's source file */
   const char *own_sym;   /* symbol present only in `path` */
   const char *other_sym; /* the other group's symbol, absent from `path` */
   const char *root;
   int iters;
   int failed;
} sa_tls_worker_arg_t;

static void *sa_tls_worker(void *argp)
{
   sa_tls_worker_arg_t *a = (sa_tls_worker_arg_t *)argp;
   for (int i = 0; i < a->iters; i++)
   {
      agent_source_authority_tls_set(1, a->root, a->path);

      cJSON *own = cJSON_CreateArray();
      int own_hits = agent_source_append_overlay_code_hits(own, a->own_sym, NULL, 5);
      cJSON *other = cJSON_CreateArray();
      int other_hits = agent_source_append_overlay_code_hits(other, a->other_sym, NULL, 5);

      if (own_hits < 1 || other_hits != 0)
         a->failed = 1; /* TLS leaked from another thread */
      cJSON_Delete(own);
      cJSON_Delete(other);
   }
   /* Release the heap-held paths for this thread (zeroed snapshot frees + clears). */
   agent_source_authority_snapshot_t z;
   memset(&z, 0, sizeof(z));
   agent_source_authority_tls_restore(&z);
   return NULL;
}

static void test_source_authority_tls_thread_isolation(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-sa-tls-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path_a[768], path_b[768];
   snprintf(path_a, sizeof(path_a), "%s/group_a.c", tmpdir);
   snprintf(path_b, sizeof(path_b), "%s/group_b.c", tmpdir);
   FILE *fa = fopen(path_a, "w");
   assert(fa);
   fputs("int alpha_unique_marker(void) { return 1; }\n", fa);
   fclose(fa);
   FILE *fb = fopen(path_b, "w");
   assert(fb);
   fputs("int beta_unique_marker(void) { return 2; }\n", fb);
   fclose(fb);

   enum
   {
      NW = 8,
      ITERS = 150
   };
   pthread_t th[NW];
   sa_tls_worker_arg_t args[NW];
   for (int i = 0; i < NW; i++)
   {
      int is_a = (i % 2 == 0);
      args[i].path = is_a ? path_a : path_b;
      args[i].own_sym = is_a ? "alpha_unique_marker" : "beta_unique_marker";
      args[i].other_sym = is_a ? "beta_unique_marker" : "alpha_unique_marker";
      args[i].root = tmpdir;
      args[i].iters = ITERS;
      args[i].failed = 0;
      assert(pthread_create(&th[i], NULL, sa_tls_worker, &args[i]) == 0);
   }
   for (int i = 0; i < NW; i++)
   {
      pthread_join(th[i], NULL);
      assert(args[i].failed == 0); /* no cross-thread source-authority leak */
   }

   unlink(path_a);
   unlink(path_b);
   rmdir(tmpdir);
}

static void test_source_authority_overlay_tools(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-source-overlay-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[768];
   snprintf(path, sizeof(path), "%s/current_source.c", tmpdir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("int overlay_unique_symbol(void) { return 42; }\n", f);
   fclose(f);

   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "1") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", path) == 0);
   assert(platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", tmpdir) == 0);

   char *result = tool_code_search("overlay_unique_symbol", NULL, 5);
   assert(result != NULL);
   cJSON *arr = parse_json_or_die(result);
   assert(cJSON_IsArray(arr));
   cJSON *first = cJSON_GetArrayItem(arr, 0);
   assert(first != NULL);
   assert(strcmp(cJSON_GetObjectItem(first, "project")->valuestring, "current_overlay") == 0);
   assert(strcmp(cJSON_GetObjectItem(first, "freshness")->valuestring, "source_packet_current") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(first, "authority")->valuestring, "current_source") == 0);
   cJSON_Delete(arr);
   free(result);

   result = tool_find_symbol("overlay_unique_symbol");
   assert(result != NULL);
   assert(strstr(result, "source_packet_current") != NULL);
   assert(strstr(result, "authority=current_source") != NULL);
   free(result);

   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", "") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", "") == 0);
   unlink(path);
   rmdir(tmpdir);
}

static void test_parse_openai_tool_calls(void)
{
   /* Build a mock OpenAI response with tool_calls */
   const char *mock_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"tool_calls\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": null,"
                           "      \"tool_calls\": [{"
                           "        \"id\": \"call_abc123\","
                           "        \"type\": \"function\","
                           "        \"function\": {"
                           "          \"name\": \"bash\","
                           "          \"arguments\": \"{\\\"command\\\":\\\"ls\\\"}\""
                           "        }"
                           "      }]"
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 100, \"completion_tokens\": 50}"
                           "}";

   /* We need to call the internal parse function. Since it's static,
    * we test indirectly by verifying the cJSON structure matches what
    * the parser expects. This validates the JSON format contract. */
   cJSON *root = cJSON_Parse(mock_json);
   assert(root != NULL);

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   assert(choices && cJSON_GetArraySize(choices) == 1);

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "tool_calls") == 0);

   cJSON *message = cJSON_GetObjectItem(choice, "message");
   cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
   assert(tool_calls && cJSON_GetArraySize(tool_calls) == 1);

   cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
   cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
   assert(tc_id && strcmp(tc_id->valuestring, "call_abc123") == 0);

   cJSON *fn = cJSON_GetObjectItem(tc, "function");
   cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
   assert(fn_name && strcmp(fn_name->valuestring, "bash") == 0);

   cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
   assert(fn_args && cJSON_IsString(fn_args));
   cJSON *args_parsed = cJSON_Parse(fn_args->valuestring);
   assert(args_parsed != NULL);
   cJSON *cmd = cJSON_GetObjectItem(args_parsed, "command");
   assert(cmd && strcmp(cmd->valuestring, "ls") == 0);
   cJSON_Delete(args_parsed);

   cJSON_Delete(root);

   /* Test a text response (no tool calls) */
   const char *text_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"stop\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": \"Task complete.\""
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 200, \"completion_tokens\": 10}"
                           "}";

   root = cJSON_Parse(text_json);
   assert(root != NULL);
   choices = cJSON_GetObjectItem(root, "choices");
   choice = cJSON_GetArrayItem(choices, 0);
   finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "stop") == 0);
   message = cJSON_GetObjectItem(choice, "message");
   cJSON *content = cJSON_GetObjectItem(message, "content");
   assert(content && strcmp(content->valuestring, "Task complete.") == 0);
   cJSON_Delete(root);
}

/* --- Shared path-policy tests (traversal and sensitive-path rejection) --- */

static void test_path_traversal_rejected(void)
{
   /* read_file with traversal should be rejected */
   char *result = tool_read_file("/tmp/../etc/shadow", 0, 0, 1);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* write_file with traversal */
   result = tool_write_file("/tmp/../etc/shadow", "hacked");
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* dispatch traversal via read_file */
   result = dispatch_tool_call("read_file", "{\"path\":\"/tmp/../../etc/shadow\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* list_files with traversal in path */
   result = tool_list_files("/tmp/../etc", NULL);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);
}

static void test_sensitive_path_rejected(void)
{
   /* read_file: .ssh directory */
   char ssh_path[256];
   const char *home = getenv("HOME");
   if (home)
   {
      snprintf(ssh_path, sizeof(ssh_path), "%s/.ssh/id_rsa", home);
      char *result = tool_read_file(ssh_path, 0, 0, 1);
      assert(result != NULL);
      /* Should be rejected as sensitive or fail to open */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
   }
}

static void test_symlink_escape_rejected(void)
{
   /* Create a symlink pointing to /etc/shadow and try to read through it */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee_test_symlink_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char link_path[512];
   snprintf(link_path, sizeof(link_path), "%s/sneaky_link", tmpdir);

   /* Create symlink to a sensitive path */
   if (symlink("/etc/shadow", link_path) == 0)
   {
      char *result = tool_read_file(link_path, 0, 0, 1);
      assert(result != NULL);
      /* Should be blocked by the realpath-based check */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
      unlink(link_path);
   }

   rmdir(tmpdir);
}

/* --- messages_compact_consecutive tests --- */

static cJSON *make_msg(const char *role, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", role);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

static void test_compact_empty(void)
{
   assert(messages_compact_consecutive(NULL) == 0);
   cJSON *arr = cJSON_CreateArray();
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 0);
   cJSON_Delete(arr);
}

static void test_compact_single(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 1);
   cJSON_Delete(arr);
}

static void test_compact_two_same_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   cJSON_AddItemToArray(arr, make_msg("user", "world"));
   assert(messages_compact_consecutive(arr) == 1);
   assert(cJSON_GetArraySize(arr) == 1);
   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "hello\n\nworld") == 0);
   cJSON_Delete(arr);
}

static void test_compact_five_same_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "a"));
   cJSON_AddItemToArray(arr, make_msg("user", "b"));
   cJSON_AddItemToArray(arr, make_msg("user", "c"));
   cJSON_AddItemToArray(arr, make_msg("user", "d"));
   cJSON_AddItemToArray(arr, make_msg("user", "e"));
   assert(messages_compact_consecutive(arr) == 4);
   assert(cJSON_GetArraySize(arr) == 1);
   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "a\n\nb\n\nc\n\nd\n\ne") == 0);
   cJSON_Delete(arr);
}

static void test_compact_mixed_roles(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "u1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u2"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "a1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u3"));
   cJSON_AddItemToArray(arr, make_msg("user", "u4"));
   assert(messages_compact_consecutive(arr) == 2);
   assert(cJSON_GetArraySize(arr) == 3);
   const char *c0 =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   const char *r1 = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "role"));
   const char *c2 =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "content"));
   assert(strcmp(c0, "u1\n\nu2") == 0);
   assert(strcmp(r1, "assistant") == 0);
   assert(strcmp(c2, "u3\n\nu4") == 0);
   cJSON_Delete(arr);
}

static void test_compact_no_consecutive(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "u1"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "a1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u2"));
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_compact_idempotent(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "a"));
   cJSON_AddItemToArray(arr, make_msg("user", "b"));
   messages_compact_consecutive(arr);
   assert(cJSON_GetArraySize(arr) == 1);
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 1);
   cJSON_Delete(arr);
}

static void test_compact_skips_structured_content(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *msg1 = cJSON_CreateObject();
   cJSON_AddStringToObject(msg1, "role", "assistant");
   cJSON_AddNullToObject(msg1, "content");
   cJSON_AddItemToArray(arr, msg1);
   cJSON_AddItemToArray(arr, make_msg("assistant", "text response"));
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 2);
   cJSON_Delete(arr);
}

static void test_compact_skips_openai_tool_results(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "run two tools"));
   cJSON *assistant = cJSON_CreateObject();
   cJSON_AddStringToObject(assistant, "role", "assistant");
   cJSON_AddNullToObject(assistant, "content");
   cJSON *tool_calls = cJSON_AddArrayToObject(assistant, "tool_calls");
   for (int i = 0; i < 2; i++)
   {
      char id[16];
      snprintf(id, sizeof(id), "call_%d", i + 1);
      cJSON *tc = cJSON_CreateObject();
      cJSON_AddStringToObject(tc, "id", id);
      cJSON_AddStringToObject(tc, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(tc, "function");
      cJSON_AddStringToObject(fn, "name", "bash");
      cJSON_AddStringToObject(fn, "arguments", "{}");
      cJSON_AddItemToArray(tool_calls, tc);
   }
   cJSON_AddItemToArray(arr, assistant);
   for (int i = 0; i < 2; i++)
   {
      char id[16];
      char content[16];
      snprintf(id, sizeof(id), "call_%d", i + 1);
      snprintf(content, sizeof(content), "result_%d", i + 1);
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "role", "tool");
      cJSON_AddStringToObject(tool, "tool_call_id", id);
      cJSON_AddStringToObject(tool, "content", content);
      cJSON_AddItemToArray(arr, tool);
   }
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 4);
   cJSON *tool1 = cJSON_GetArrayItem(arr, 2);
   cJSON *tool2 = cJSON_GetArrayItem(arr, 3);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(tool1, "tool_call_id")), "call_1") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(tool2, "tool_call_id")), "call_2") == 0);
   cJSON_Delete(arr);
}

static void test_compact_system_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("system", "rule1"));
   cJSON_AddItemToArray(arr, make_msg("system", "rule2"));
   assert(messages_compact_consecutive(arr) == 1);
   assert(cJSON_GetArraySize(arr) == 1);
   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "rule1\n\nrule2") == 0);
   cJSON_Delete(arr);
}

static void test_delegation_error_guidance(void)
{
   char buf[512];

   /* Known patterns return guidance */
   assert(delegation_error_guidance("missing prompt", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "non-empty prompt") != NULL);

   assert(delegation_error_guidance("prompt too short (5 chars)", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "too brief") != NULL);

   assert(delegation_error_guidance("no agent available for role 'xyzzy'", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "valid role") != NULL);

   assert(delegation_error_guidance("missing delegation_id or content", buf, sizeof(buf)) == 1);
   assert(strstr(buf, "delegate_reply") != NULL);

   assert(delegation_error_guidance("delegation depth limit exceeded (3/3)", buf, sizeof(buf)) ==
          1);
   assert(strstr(buf, "max_delegation_depth") != NULL);

   assert(delegation_error_guidance(
              "delegation spawn limit exceeded (51/50 nested delegates for root)", buf,
              sizeof(buf)) == 1);
   assert(strstr(buf, "max_delegation_spawns") != NULL);
   assert(strstr(buf, "sub-delegation") != NULL);

   /* Unknown errors produce no guidance */
   assert(delegation_error_guidance("internal error", buf, sizeof(buf)) == 0);
   assert(buf[0] == '\0');

   /* NULL / empty inputs */
   assert(delegation_error_guidance(NULL, buf, sizeof(buf)) == 0);
   assert(delegation_error_guidance("missing prompt", NULL, 0) == 0);
   assert(delegation_error_guidance("missing prompt", buf, 0) == 0);
}

static void test_agent_trace_log_uses_db1_execution_trace(void)
{
   /* Started once from main, before anything else touches the bus. */
   if (!store_module_fixture_available())
      return;

   /* The real Postgres-backed store enforces execution_trace.plan_id as a
    * foreign key. Seed the plan this trace declares instead of relying on the
    * old in-process stub's acceptance of a fabricated id. The assertion remains
    * about agent_trace_log mapping turn and tool_name into the stored row. */
   int plan_id = db1_execution_plan_create("trace-fixture", "trace contract", "[]");
   assert(plan_id > 0);
   agent_trace_log(plan_id, 3, "call", "content", "bash", "{}", "ok", "abc123");

   db1_execution_trace_recent_row_t rows[4];
   int count = db1_execution_trace_list_recent(rows, 4);
   assert(count == 1);
   assert(rows[0].turn == 3);
   assert(strcmp(rows[0].tool_name, "bash") == 0);
}

static void test_agent_endpoint_valid(void)
{
   /* `agent add` is positional, so a flag in the endpoint slot used to be SAVED as the
    * endpoint: the agent reported ON and returned success, and the only symptom was
    * `agent probe` reporting "GET --provider/models returned -1" later. */
   assert(!agent_endpoint_valid("--provider"));
   assert(!agent_endpoint_valid("-x"));
   assert(!agent_endpoint_valid(""));
   assert(!agent_endpoint_valid(NULL));

   /* Narrow on purpose: everything that is not obviously a flag stays accepted,
    * including the scheme-less host:port forms this command has always taken. */
   assert(agent_endpoint_valid("http://127.0.0.1:8080/v1"));
   assert(agent_endpoint_valid("https://api.openai.com/v1"));
   assert(agent_endpoint_valid("localhost:11434"));
   assert(agent_endpoint_valid("wizard-llm:8080/v1"));
}

static void test_agent_name_valid(void)
{
   /* legit agent/model slugs accepted */
   assert(agent_name_valid("minimax"));
   assert(agent_name_valid("mimo-2.5"));
   assert(agent_name_valid("my-gpt"));
   assert(agent_name_valid("a"));
   assert(agent_name_valid("gpt_4.1-mini"));
   /* junk rejected: empty, leading non-alnum, spaces, illegal chars, over-long */
   assert(!agent_name_valid(""));
   assert(!agent_name_valid(NULL));
   assert(!agent_name_valid("-leading"));
   assert(!agent_name_valid(".dot"));
   assert(!agent_name_valid("has space"));
   assert(!agent_name_valid("bad/slash"));
   assert(!agent_name_valid(
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); /* 63 */
}

/* Layer 2 of the session-isolation guard: the server-side write chokepoint
 * agent_tools_session_isolation_blocks. Default-off, and when on blocks writes
 * whose normalized target is outside an aimee-managed worktree. */
static void test_session_isolation_guard(void)
{
   const char *current_home = getenv("AIMEE_HOME");
   char *saved_aimee_home = current_home ? strdup(current_home) : NULL;
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee_sess_iso_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   char cfg[640];
   snprintf(cfg, sizeof(cfg), "%s/aimee.yaml", home);
   assert(platform_setenv("AIMEE_HOME", home) == 0);

   const char *primary = "/some/repo/src/x.c";
   const char *worktree = "/some/repo/.aimee/worktrees/ab12/main/src/x.c";

   /* Default ON, including when config authority is unavailable: a
    * primary-checkout target is blocked. */
   remove(cfg);
   assert(agent_tools_session_isolation_blocks(primary, NULL) == 1);

   /* Only the managed worktree that owns cwd is allowed. */
   assert(agent_tools_session_isolation_blocks(primary, NULL) == 1);
   assert(agent_tools_session_isolation_blocks(worktree, NULL) == 1);
   assert(agent_tools_session_isolation_blocks(worktree, "/some/repo/.aimee/worktrees/ab12/main") ==
          0);
   assert(agent_tools_session_isolation_blocks("/some/repo/.aimee/worktrees/other/main/src/x.c",
                                               "/some/repo/.aimee/worktrees/ab12/main") == 1);
   /* Traversal escape out of the worktree normalizes + blocks. */
   assert(agent_tools_session_isolation_blocks(
              "/some/repo/.aimee/worktrees/ab12/main/../../../src/y.c", NULL) == 1);
   /* Relative path resolved against cwd (normalize_path): a worktree cwd allows,
    * a primary-checkout cwd blocks. */
   assert(agent_tools_session_isolation_blocks("src/x.c",
                                               "/some/repo/.aimee/worktrees/ab12/main") == 0);
   assert(agent_tools_session_isolation_blocks("src/x.c", "/some/repo") == 1);
   /* The workflow engine's per-work-item worktrees ARE managed isolation (the
    * server-side mirror of #1314's guardrail fix): a wfe delegate's writes into
    * its own wfe worktree must not be refused by this backstop. */
   assert(agent_tools_session_isolation_blocks("/var/lib/aimee/wfe-worktrees/wi_ab12.s3/src/x.c",
                                               "/var/lib/aimee/wfe-worktrees/wi_ab12.s3") == 0);
   assert(agent_tools_session_isolation_blocks("/var/lib/aimee/wfe-worktrees/wi_other.s3/src/x.c",
                                               "/var/lib/aimee/wfe-worktrees/wi_ab12.s3") == 1);
   assert(agent_tools_session_isolation_blocks("src/x.c",
                                               "/var/lib/aimee/wfe-worktrees/wi_ab12.s3") == 0);
   /* Traversal OUT of a wfe worktree still blocks. */
   assert(agent_tools_session_isolation_blocks(
              "/var/lib/aimee/wfe-worktrees/wi_ab12.s3/../../escape.c", NULL) == 1);
   /* NULL path is a no-op (returns 0). */
   assert(agent_tools_session_isolation_blocks(NULL, NULL) == 0);

   remove(cfg);
   if (saved_aimee_home)
   {
      assert(platform_setenv("AIMEE_HOME", saved_aimee_home) == 0);
      free(saved_aimee_home);
   }
   else
      assert(platform_unsetenv("AIMEE_HOME") == 0);
   printf("session_isolation guard (Layer 2) OK\n");
}

/* A rewritten agents.json whose mtime did not advance must still be seen.
 * mtime alone is not a safe cache key: it is neither monotonic nor guaranteed
 * distinct across a rewrite. Observed live on the appliance's tiered
 * filesystem, where a reinstalled agents.json landed with an mtime ~9h in the
 * past and every /v1/agents request kept failing (502) and /v1/model/list kept
 * returning an empty array until the file was touched. Size+inode come free
 * from the same stat() and make the rewrite detectable. */
static void test_agent_config_cache_detects_same_mtime_rewrite(void)
{
   struct stat st;
   const char *path = agent_config_path();

   /* The suite runs with AIMEE_NO_CACHE=1, which disables the cache path
    * entirely. This test is ABOUT that path, so turn caching back on for the
    * duration -- otherwise it passes without exercising anything. */
   unsetenv("AIMEE_NO_CACHE");

   {
      FILE *f = fopen(path, "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"before\",\"provider\":\"anthropic\","
            "\"model\":\"m\",\"roles\":[\"code\"]}]}\n",
            f);
      fclose(f);
   }

   agent_config_t first;
   assert(agent_load_config(&first) == 0);
   assert(first.agent_count == 1);
   assert(agent_find(&first, "before") != NULL);
   assert(stat(path, &st) == 0);
   struct timespec pinned = st.st_mtim;

   /* Rewrite with different content, then pin the mtime back to what the cache
    * already saw -- the exact situation a coarse/non-monotonic filesystem
    * produces on its own. */
   {
      FILE *f = fopen(path, "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"after_one\",\"provider\":\"anthropic\","
            "\"model\":\"m\",\"roles\":[\"code\"]},"
            "{\"name\":\"after_two\",\"provider\":\"anthropic\","
            "\"model\":\"m\",\"roles\":[\"code\"]}]}\n",
            f);
      fclose(f);
   }
   {
      struct timespec times[2];
      times[0] = pinned; /* atime */
      times[1] = pinned; /* mtime: rewind to the cached value */
      assert(utimensat(AT_FDCWD, path, times, 0) == 0);
      assert(stat(path, &st) == 0);
      assert(st.st_mtim.tv_sec == pinned.tv_sec && st.st_mtim.tv_nsec == pinned.tv_nsec);
   }

   /* The cache must NOT hand back the stale single-agent config. */
   agent_config_t second;
   assert(agent_load_config(&second) == 0);
   assert(second.agent_count == 2);
   assert(agent_find(&second, "after_one") != NULL);
   assert(agent_find(&second, "after_two") != NULL);
   assert(agent_find(&second, "before") == NULL);

   unlink(path);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0); /* restore suite default */
   printf("  PASS: test_agent_config_cache_detects_same_mtime_rewrite\n");
}

/* The agents.json deletion guard: agent_save_config must never destroy a populated
 * file, and its write must be atomic. This pins the two ways the file used to go
 * missing on the live server — an empty registry overwriting it, and a truncating
 * fopen("w") that a failed/interrupted write left at zero bytes. */
static void test_agent_config_deletion_guard(void)
{
   const char *old_home = getenv("HOME");
   const char *old_aimee_home = getenv("AIMEE_HOME");
   char saved_home[MAX_PATH_LEN] = "";
   char saved_aimee_home[MAX_PATH_LEN] = "";
   if (old_home)
      snprintf(saved_home, sizeof(saved_home), "%s", old_home);
   if (old_aimee_home)
      snprintf(saved_aimee_home, sizeof(saved_aimee_home), "%s", old_aimee_home);

   char home[MAX_PATH_LEN];
   snprintf(home, sizeof(home), "%s/aimee-guard-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   setenv("HOME", home, 1);
   unsetenv("AIMEE_HOME");
   assert(platform_mkdir_p(config_default_dir(), 0700) == 0 ||
          access(config_default_dir(), F_OK) == 0);

   /* Seed a populated registry (2 agents). */
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "alpha");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "beta");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "openai");
   assert(agent_save_config(&cfg) == 0);

   agent_config_t chk;
   assert(agent_load_config(&chk) == 0 && chk.agent_count == 2);

   /* (1) An EMPTY registry must be REFUSED over the populated file, and the file
    * must survive untouched. This is the guard proper. */
   {
      agent_config_t empty;
      memset(&empty, 0, sizeof(empty));
      assert(agent_save_config(&empty) != 0);
      agent_config_t after;
      assert(agent_load_config(&after) == 0);
      assert(after.agent_count == 2); /* NOT wiped */
   }

   /* (2) A zero-agent save IS allowed with no existing file (fresh install). */
   {
      unlink(agent_config_path());
      agent_config_t empty;
      memset(&empty, 0, sizeof(empty));
      assert(agent_save_config(&empty) == 0);
   }

   /* (3) Atomic write leaves no <path>.tmp.* behind on success. */
   {
      assert(agent_save_config(&cfg) == 0);
      char cmd[MAX_PATH_LEN + 64];
      snprintf(cmd, sizeof(cmd), "ls %s.tmp.* >/dev/null 2>&1", agent_config_path());
      assert(system(cmd) != 0);
   }

   /* (4) Removing the LAST agent empties the registry legitimately, and the guard
    * must not refuse it. It looks identical to case (1) from inside the save, so
    * the remove path declares itself; without that, deleting the only configured
    * delegate failed with "could not save agents.json" and it could never be
    * removed. Removing one of several was unaffected, which is why this hid. */
   {
      assert(agent_save_config(&cfg) == 0); /* 2 agents on disk */
      agent_config_t one;
      assert(agent_load_config(&one) == 0 && one.agent_count == 2);
      one.agent_count = 1; /* drop beta, as handle_agent_remove does */
      assert(agent_save_config(&one) == 0);

      agent_config_t last;
      assert(agent_load_config(&last) == 0 && last.agent_count == 1);
      last.agent_count = 0;                                /* now drop the last one */
      assert(agent_save_config(&last) != 0);               /* plain save still refuses */
      assert(agent_save_config_after_removal(&last) == 0); /* the removal path may */

      agent_config_t gone;
      assert(agent_load_config(&gone) == 0 && gone.agent_count == 0);
   }

   /* (5) The exemption is scoped to removal: a zeroed cfg from any other caller is
    * still refused over a populated file. */
   {
      assert(agent_save_config(&cfg) == 0);
      agent_config_t zeroed;
      memset(&zeroed, 0, sizeof(zeroed));
      assert(agent_save_config(&zeroed) != 0);
      agent_config_t after;
      assert(agent_load_config(&after) == 0 && after.agent_count == 2);
   }

   if (old_home)
      assert(platform_setenv("HOME", saved_home) == 0);
   else
      assert(platform_unsetenv("HOME") == 0);
   if (old_aimee_home)
      assert(platform_setenv("AIMEE_HOME", saved_aimee_home) == 0);
   else
      assert(platform_unsetenv("AIMEE_HOME") == 0);
   platform_test_rmrf(home);
   printf("  PASS: agent_config_deletion_guard\n");
}

/* Saving a config must not seed the load cache with the caller's struct.
 *
 * Every writer (the setup wizard's /v1 agent-add included) builds an agent_t
 * from request fields and calls agent_save_config(); NONE of them run the
 * normalisation that agent_load_config() owns — provider defaulting, the wire
 * rewrite, agent_derive_catalog_provider(), the tier/capability pass. The save
 * path used to memcpy that raw struct into the cache and stamp it with the
 * freshly written file's stat, so the very next load was a cache HIT on an
 * unnormalised config and stayed one for the life of the process.
 *
 * The user-visible damage was silent: a wizard-added Kimi agent kept an empty
 * catalog_provider, so agent_catalog_provider() fell back to the wire name
 * "openai". That cost it the {moonshotai,k3,1.0} required-temperature row (the
 * delegate's default 0.3 reached a model that accepts only 1, failing every
 * call with "invalid temperature: only 1 is allowed for this model") and made
 * model_capability_get() miss, so it advertised no tools and tool roles
 * rejected it as unavailable. Only a restart cleared it. */
static void test_agent_save_config_does_not_cache_underived_agents(void)
{
   /* The bug is invisible when the cache is bypassed, so assert on the cached
    * path explicitly rather than inheriting whatever the harness set. */
   platform_unsetenv("AIMEE_NO_CACHE");
   unlink(agent_config_path());

   /* Exactly what a writer hands to agent_save_config: endpoint and model set,
    * no provider and no catalog_provider — both are load-side derivations. */
   agent_config_t built;
   memset(&built, 0, sizeof(built));
   built.agent_count = 1;
   snprintf(built.default_agent, sizeof(built.default_agent), "%s", "Kimi");
   snprintf(built.agents[0].name, sizeof(built.agents[0].name), "%s", "Kimi");
   snprintf(built.agents[0].endpoint, sizeof(built.agents[0].endpoint), "%s",
            "https://api.kimi.com/coding/v1");
   snprintf(built.agents[0].model, sizeof(built.agents[0].model), "%s", "kimi-k3");
   snprintf(built.agents[0].auth_type, sizeof(built.agents[0].auth_type), "%s", "bearer");
   snprintf(built.agents[0].api_key, sizeof(built.agents[0].api_key), "%s", "k");
   snprintf(built.agents[0].roles[0], sizeof(built.agents[0].roles[0]), "%s", "review");
   built.agents[0].role_count = 1;
   built.agents[0].enabled = 1;
   assert(built.agents[0].catalog_provider[0] == '\0');

   assert(agent_save_config(&built) == 0);

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   const agent_t *kimi = agent_find(&loaded, "Kimi");
   assert(kimi != NULL);

   /* The load must have re-parsed and derived. Asserting the raw field rather
    * than agent_catalog_provider() is deliberate: the accessor falls back to
    * ->provider, which would mask an empty field behind the wire name — and it
    * is the RAW field that model_sampling.c's required-temperature gate reads. */
   assert(strcmp(kimi->catalog_provider, "moonshotai") == 0);

   /* The wire provider is still the load-side default, untouched by derivation. */
   assert(strcmp(kimi->provider, "openai") == 0);

   unlink(agent_config_path());
   printf("  PASS: test_agent_save_config_does_not_cache_underived_agents\n");
}

int main(void)
{
   /* BEFORE ANYTHING ELSE, because the fixture configures the module runtime and
      starts the bus, and obs_bus refuses to be configured once it is running.
      Several tests below bring the bus up as a side effect, so a fixture started
      at the first store-backed test finds it already up and dies -- which is how
      this was found: with AIMEE_STORE_URL set, the suite aborted at
      "configure the module endpoint" rather than skipping.
      Ordering, not availability, and only a run with a real database shows it. */
   if (store_module_fixture_available())
      store_module_fixture_start();

   delegate_role_seam_install();
   char tmp_home[512];
   snprintf(tmp_home, sizeof(tmp_home), "%s/aimee-test-agent-home-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmp_home) != NULL);
   assert(platform_setenv("HOME", tmp_home) == 0);
   /* AIMEE_HOME OVERRIDES HOME in aimee_home(), so sandboxing HOME alone is a
    * no-op anywhere AIMEE_HOME is set - which is every deployed server (the
    * appliance runs with AIMEE_HOME=/var/lib/aimee). Tests below write to and
    * unlink() agent_config_path(); without this line they destroy the
    * operator's real agents.json. Set it before any config or agent call. */
   assert(platform_setenv("AIMEE_HOME", tmp_home) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   char audit_db[640];
   snprintf(audit_db, sizeof(audit_db), "%s/audit-worm.db", tmp_home);
   assert(audit_ensure_key() == 0);
   assert(audit_worm_init_at(audit_db) == 0);
   assert(config_output_dir()[0] != '\0');

   /* Guard the sandbox itself: if agent_config_path() ever resolves outside the
    * temp home, this suite is about to eat real operator config. Fail loudly
    * here rather than silently deleting it. */
   assert(strncmp(agent_config_path(), tmp_home, strlen(tmp_home)) == 0);
   session_id_set_override("unit-test-agent");
   test_tool_surface_single_source();
   test_agent_endpoint_valid();
   test_agent_name_valid();
   test_agent_expand_env();
   test_agent_save_never_serializes_literal_key();
   test_agent_has_role();
   test_agent_supports_persona();
   test_agent_find();
   test_agent_route();
   test_agent_route_selection_provider();
   test_route_authority_refuses_without_provider();
   test_agent_route_policy_filter();
   test_agent_route_primary_turn_marker();
   test_agent_route_client_only_claude_excluded();
   test_agent_routing_block_reason();
   test_agent_route_with_caps_honors_tools_enabled();
   test_agent_route_with_caps_honors_context_override();
   test_knowledge_write_gates_the_tool_policy();
   test_withheld_knowledge_write_blocks_stale_context_tools();
   test_a_delegate_without_shell_cannot_run_commands();
   test_permissions_clamp_the_toolset();
   test_one_delegates_permissions_are_not_anothers();
   test_provider_env_credentials_and_headers();
   test_codex_oauth_request_creds();
   test_codex_oauth_reads_vault_only();
   test_request_creds_snapshot_carries_vault_principal();
   test_agent_config_provider_cli_roundtrip();
   test_tools_enabled_capability_default();
   test_agent_default_primary_skips_disabled();
   test_catalog_provider_separates_vendor_from_wire();
   test_catalog_provider_explicit_round_trip();
   test_unknown_context_window_does_not_pass_min_context();
   test_context_window_table_covers_live_vendors();
   test_agent_save_config_does_not_cache_underived_agents();
   test_catalog_provider_host_matching_is_label_anchored();
   test_catalog_provider_namespaced_model_ids();
   test_moonshot_heuristic_scopes_reasoning_to_known_families();
   test_catalog_provider_maps_cli_provider_names();
   test_primary_turn_reaches_default_above_min_tier();
   test_primary_turn_default_must_still_satisfy_caps();
   test_catalog_provider_endpoint_parser_edges();
   test_request_max_tokens_clamped_to_context_window();
   test_registration_prefix();
   test_registration_grouping();
   test_declared_roles_route_precisely();
   test_scope_ceiling_matches_work_to_capability();
   test_escalation_target_selection();
   test_prefer_local_orders_but_never_bypasses();
   test_prefer_healthy_over_degraded();
   test_provider_general_registration_expands();
   test_provider_general_preserves_explicit_catalog_provider();
   test_provider_general_overflow_rejects_config();
   test_provider_general_auto_uses_curated_allowlist();
   test_provider_general_auto_requires_curated_set();
   test_provider_general_rejects_malformed_registrations();
   test_capability_routing_flag_behaviour_diff();
   test_capability_gate_escalates_instead_of_failing();
   test_no_escalation_when_capability_routing_disabled();
   test_escalation_respects_policy_and_health_gates();
   test_agent_config_cache_detects_same_mtime_rewrite();
   test_agent_adapter_registry();
   test_agent_config_deletion_guard();
   test_local_synth_not_masked_by_tmux_codex();
   test_provider_cli_shell_exec_uses_argv_not_shell();
   test_provider_cli_shell_timeout_covers_prompt_write();
   test_codex_oauth_auth_resolution();
   test_responses_parser_keeps_all_output_text_parts();
   test_responses_parser_accumulates_output_text_deltas();
   test_responses_object_folds_in_delta_text();
   test_responses_object_folds_in_streamed_function_call();
   test_responses_object_keeps_existing_function_call();
   test_responses_object_keeps_existing_text();
   test_ir_parse_responses_tool_call();
   test_ir_parse_responses_namespaced_tool_call();
   test_ir_parse_responses_text_only();
   test_responses_parser_uses_output_text_done();
   test_responses_parser_separates_message_items();
   test_agent_is_exec_role();
   test_tool_bash();
   test_tool_bash_delegate_unsandboxed_refused();
   test_detached_skips_worktree_rewrite();
   test_tool_read_file();
   test_tool_write_file();
   test_tool_edit_file();
   test_tool_edit_file_anchored();
   test_anchored_large_span_advisory();
   test_part3_anchored_tools();
   test_web_read_ssrf();
   test_parent_write_guard_blocks_parent_writes();
   test_session_isolation_guard();
   test_parent_write_guard_readonly_pipeline();
   test_stale_parent_guard_blocks_other_worktree_then_clear_unblocks();
   test_parent_write_guard_allows_mkdir_in_delegate_worktree();
   test_parent_write_guard_allows_workspace_file_ops();
   test_parent_write_guard_allows_workspace_chain();
   test_parent_write_guard_allows_readonly_printf();
   test_detached_dead_channel_reports_clear_error();
   test_container_bash_runs_in_sandbox();
   test_container_execute_script_runs_in_sandbox();
   test_parent_write_guard_shell_uses_delegate_cwd_in_git_parent();
   test_git_tools_default_to_session_worktree();
   test_tool_list_files();
   test_tool_grep_excludes_heavy_dirs();
   test_dispatch_tool_call();
   test_source_authority_overlay_tools();
   test_source_authority_tls_thread_isolation();
   test_parse_openai_tool_calls();
   test_path_traversal_rejected();
   test_sensitive_path_rejected();
   test_symlink_escape_rejected();
   test_compact_empty();
   test_compact_single();
   test_compact_two_same_role();
   test_compact_five_same_role();
   test_compact_mixed_roles();
   test_compact_no_consecutive();
   test_compact_idempotent();
   test_compact_skips_structured_content();
   test_compact_skips_openai_tool_results();
   test_compact_system_role();
   test_delegation_error_guidance();
   test_cancelled_durable_job_blocks_tool_dispatch();
   test_shell_worktree_rewrite_is_applied_not_refused();
   test_delegate_bash_cancel_kills_running_tool();
   test_parent_write_guard_readonly_large_find();
   test_agent_trace_log_uses_db1_execution_trace();

   /* Kill any aimee-kb daemon these tests autostarted under tmp_home and remove
    * the scratch home, so the daemon is not orphaned. Leaked aimee-kb daemons
    * retry DB2 provisioning and were the source of the stuck-`createdb` runaway
    * (#2569); tmp_home is a unique mkdtemp path, so this targets only this
    * test's own daemon. */
   {
      audit_worm_close();
      char cmd[640];
      snprintf(cmd, sizeof(cmd), "pkill -KILL -f 'aimee-kb --socket=%s' 2>/dev/null", tmp_home);
      (void)system(cmd);
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_home);
      (void)system(cmd);
   }
   printf("agent: all tests passed\n");
   return 0;
}

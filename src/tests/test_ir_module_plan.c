/* Generic IR plan execution against the production host connections. Go owns
 * plan selection; these fixed plan fixtures exercise rendered byte identity,
 * typed authority, audit connections and fail-atomic validation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aimee/ir/module_plan.h>
#include "server/ir_host_bindings.h"
#include "ingress_preinject.h"
#include "cJSON.h"
#include "config.h"
#include "platform_test_util.h"
#include "kb_client.h"
#include "support/module_runtime_fixture.h"

/* When set, the recall stubs return nothing so ingress_preinject_build → NULL
 * (the "pre-injection off / recall empty" path). */
static int g_no_recall = 0;
static int g_test_placement =
    0; /* drives ingress_cache_placement_enabled in legacy_config_read stub */

int config_present(void)
{
   return 0;
}
int config_integrity_enabled(void)
{
   return 1;
}
int config_integrity_dry_run(void)
{
   return 0;
}

/* The policy is tested in Go. Here the host consumes fixture plans and must
 * preserve typed authority, query selection and complete rendered bytes. */
static int fixture_session_start = 1;
static const char *fixture_query = "deploy matrix";
static const char *fixture_remove_tools = "[]";
static int fixture_transport_result = 1;
static const char *fixture_gate;
static int fixture_audits;
static const char *const *fixture_provided_resources;
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *request,
                                                    int timeout_ms, cJSON **result)
{
   assert(timeout_ms == 500);
   assert(strcmp(method, "memory.runtime") == 0 && cJSON_IsObject(request));
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "operation"));
   assert(operation);
   if (strncmp(operation, "ingress-", 8) == 0)
      return module_runtime_fixture_call(request, result);
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   assert(strcmp(operation, "gateway-plan") == 0);
   const char *phase = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "phase"));
   assert(phase);
   const cJSON *provided = cJSON_GetObjectItemCaseSensitive(request, "provided_resources");
   if (fixture_provided_resources)
   {
      assert(cJSON_GetArraySize(provided) == 1);
      assert(strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(provided, 0)), "guidance") == 0);
   }
   else
      assert(!provided);
   if (strcmp(phase, "text") != 0)
   {
      const cJSON *roles = cJSON_GetObjectItemCaseSensitive(request, "roles");
      const cJSON *tools = cJSON_GetObjectItemCaseSensitive(request, "tools");
      assert(cJSON_IsArray(roles) && cJSON_IsArray(tools));
      assert(strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(roles, 0)), "user") == 0);
      if (!fixture_session_start)
         assert(strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(roles, 1)), "assistant") == 0);
   }
   if (fixture_gate)
   {
      cJSON_Delete(*result);
      *result = cJSON_Parse(fixture_gate);
      return fixture_transport_result;
   }
   cJSON *steps = cJSON_AddArrayToObject(*result, "steps");
   if (strcmp(phase, "tools") == 0)
   {
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "kind", "remove_tools");
      cJSON_AddItemToObject(step, "indices", cJSON_Parse(fixture_remove_tools));
      cJSON_AddItemToArray(steps, step);
   }
   else
   {
      const char *supplied =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "provided_query"));
      const char *fallback =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "last_user_text"));
      assert((supplied && strcmp(supplied, fixture_query) == 0) ||
             (fallback && strcmp(fallback, fixture_query) == 0));
      cJSON *step = cJSON_Parse(
          "{\"kind\":\"invoke\",\"binding\":\"context\",\"output\":\"evidence\",\"args\":{}}");
      cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(step, "args"), "query",
                              fixture_query);
      cJSON_AddItemToArray(steps, step);
      cJSON_AddStringToObject(*result, "result", "evidence");
      if (strcmp(phase, "context") == 0)
      {
         if (fixture_session_start)
            cJSON_AddItemToArray(
                steps,
                cJSON_Parse("{\"kind\":\"append_context\",\"resource\":\"guidance\",\"context\":{"
                            "\"origin\":\"platform\",\"authority\":\"task_instruction\",\"trust\":"
                            "\"verified\",\"sensitivity\":\"internal\",\"model_visible\":true}}"));
         cJSON_AddItemToArray(
             steps,
             cJSON_Parse(
                 "{\"kind\":\"invoke\",\"binding\":\"epoch\",\"args\":{\"domain\":\"knowledge\","
                 "\"scope\":\"global\"},\"output\":\"epoch\",\"when_output\":\"evidence\"}"));
         cJSON_AddItemToArray(
             steps,
             cJSON_Parse("{\"kind\":\"append_context\",\"output\":\"evidence\",\"when_output\":"
                         "\"evidence\",\"epoch_output\":\"epoch\",\"context\":{\"origin\":"
                         "\"retrieval\",\"authority\":\"evidence\",\"trust\":\"unverified\","
                         "\"sensitivity\":\"internal\",\"model_visible\":true,\"revision_domain\":"
                         "\"knowledge\",\"revision_scope\":\"global\"}}"));
      }
   }
   return fixture_transport_result;
}
static int apply_plan(aimee_request_t *ir, const char *query, const char *phase)
{
   aimee_ir_module_plan_t config = {.method = "memory.runtime",
                                    .operation = "gateway-plan",
                                    .phase = phase,
                                    .provided_query = query,
                                    .provided_resources = fixture_provided_resources,
                                    .bindings = server_ir_plan_bindings,
                                    .resources = server_ir_plan_resources};
   return aimee_ir_stage_module_plan(ir, &config);
}
static int apply_context(aimee_request_t *ir, void *query)
{
   return apply_plan(ir, query, "context");
}
static int apply_tools(aimee_request_t *ir, void *unused)
{
   (void)unused;
   return apply_plan(ir, NULL, "tools");
}
static char *render_text(const char *query)
{
   return server_ir_plan_text("memory.runtime", "gateway-plan", "text", query);
}
int learning_evidence_write_retrieval_event(const char *fingerprint, const char *role,
                                            const int64_t *ids, int count, char *out, int cap)
{
   assert(strcmp(fingerprint, "opaque-digest") == 0);
   assert(strcmp(role, "RecallGateEnforcedSkip/acknowledgement") == 0);
   assert(ids == NULL && count == 0 && out == NULL && cap == 0);
   fixture_audits++;
   return -1; /* Audit outage cannot change the Go recall decision. */
}
void obs_bus_emit_durable_event(const char *event_type, const char *subject, const char *verdict,
                                const char *detail)
{
   (void)event_type;
   (void)subject;
   (void)verdict;
   (void)detail;
}

/* --- stubs: make ingress_preinject_build deterministic without the kb graph --- */

void kb_client_memory_scope_context_apply(cJSON *request)
{
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "project", "active-project");
}
static char *diagnostic_reply(const cJSON *request);
char *kb_v1_action_request(const char *method, cJSON *request)
{
   if (!strcmp(method, "memory.diagnose_scoped"))
   {
      char *raw = diagnostic_reply(request);
      cJSON_Delete(request);
      return raw;
   }
   assert(strcmp(method, "memory.facts") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "project")),
                 "active-project") == 0);
   cJSON_Delete(request);
   return strdup("{\"status\":\"ok\",\"facts\":\"\"}");
}
char *kb_client_memory_assemble_typed_context_json(const char *query, const cJSON *context_limits)
{
   assert(cJSON_IsObject(context_limits));
   assert(cJSON_GetObjectItemCaseSensitive(context_limits, "schema_version")->valueint == 1);
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(context_limits, "max_context_bytes")));
   (void)query;
   return NULL;
}

/* Typed-facts gate stub (typed_facts feature added this call to ingress_preinject.c;
 * the test link needs the symbol). Off -> the builder's facts path stays inert. */
int ingress_preinject_resolve_active_scope(char *workspace, size_t workspace_len, char *project,
                                           size_t project_len)
{
   snprintf(workspace, workspace_len, "active-workspace");
   snprintf(project, project_len, "active-project");
   return 0;
}
void kb_client_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   (void)workspace;
   (void)project;
   assert(include_all == 0);
}
void kb_client_memory_scope_context_clear(void)
{
}
char *kb_client_code_context(const char *query, const char *symbol, const char *project,
                             int *status_out)
{
   (void)query;
   (void)symbol;
   (void)project;
   if (status_out)
      *status_out = 503;
   return NULL;
}
kb_client_result_status_t kb_client_last_result_status(void)
{
   return KB_CLIENT_RESULT_OK;
}
/* Recall is QUERY-SENSITIVE on purpose. A stub that ignores its query cannot
 * tell "the stage asked the right thing" from "the stage asked the persona
 * blob", which is precisely the regression test_ir_stage_prefers_supplied_query
 * exists to catch -- and with (void)query it passed with the fix reverted.
 * Anything that does not mention the subject recalls nothing, exactly as the
 * real kb did when handed 5773 characters of persona. */
static char *diagnostic_reply(const cJSON *request)
{
   const char *query = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "query"));
   if (g_no_recall || !query || !strstr(query, "deploy") || strstr(query, "aimee-persona"))
      return strdup("{\"status\":\"ok\",\"memories\":[]}");
   return strdup("{\"status\":\"ok\",\"memories\":[{\"id\":\"101\",\"tier\":\"L2\",\"kind\":"
                 "\"fact\",\"key\":\"deploy path\",\"headline\":\"Use the deploy "
                 "matrix.\",\"preview\":\"Use the deploy matrix.\",\"score\":0.88}]}");
}
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   if (g_no_recall || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   snprintf(out[0].file_path, sizeof(out[0].file_path), "src/server/ingress_preinject.c");
   snprintf(out[0].snippet, sizeof(out[0].snippet), "builder emits a bounded context envelope");
   return 1;
}
/* Accessor stubs expose the fixture values used by the assertions below. */
int config_ingress_cache_placement_enabled(void)
{
   return g_test_placement;
}

int config_ingress_compress_enabled(void)
{
   return 0;
}

int config_ingress_compress_min_chars(void)
{
   return 0;
}

int config_ingress_preinject_assembly_budget(void)
{
   return 1200;
}

int config_ingress_preinject_enabled(void)
{
   return 1;
}

const char *config_code_context_mode(void)
{
   return "observe";
}

int config_kb_evidence_emit_enabled(void)
{
   return 0;
}
const char *config_default_dir(void)
{
   static char directory[512];
   snprintf(directory, sizeof(directory), "%s/aimee-test", platform_tmpdir());
   return directory;
}
int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)ids;
   (void)n_ids;
   return 0;
}
int kb_client_evidence_emit_retrieval_event_ex(const char *turn_id, const char *role,
                                               const char *query_fingerprint, const int64_t *ids,
                                               int n_ids, char *event_id_out, size_t event_id_len)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)ids;
   (void)n_ids;
   if (event_id_out && event_id_len > 0)
      event_id_out[0] = '\0';
   return 0;
}
void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                   const char *const *snippets, int n)
{
   (void)surface;
   (void)event_id;
   (void)ids;
   (void)snippets;
   (void)n;
}
int kb_client_evidence_merge_retrieval_event(const char *turn_id, const char *role,
                                             const char *query_fingerprint,
                                             const char *const *types, const char *const *refs,
                                             const char *const *versions, int n)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)types;
   (void)refs;
   (void)versions;
   (void)n;
   return 0;
}
/* FIXED (not varying): the envelope must render identically across two build()
 * calls so the byte-identity comparisons below are meaningful. */
int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0x5a, len);
   return 0;
}

/* OPENAI_SYSTEM_PROMPT: render_text(q) == ingress_preinject_build(q,0)
 * byte-for-byte (the raw env, no trailing "\n\n"). */
static void test_system_prompt_raw_env(void)
{
   char *sys = render_text("deploy matrix");
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(sys != NULL && direct != NULL);
   assert(strcmp(sys, direct) == 0);
   /* The legacy contract: NOT apply()-merged, so no trailing blank line. */
   size_t n = strlen(sys);
   assert(!(n >= 2 && sys[n - 1] == '\n' && sys[n - 2] == '\n'));
   free(sys);
   free(direct);
   printf("system_prompt_raw_env OK\n");
}

/* Pre-injection off / recall empty: every target is a byte-identical no-op. */
static void test_disabled_noop(void)
{
   g_no_recall = 1;
   assert(render_text("deploy matrix") == NULL);
   g_no_recall = 0;
   printf("disabled_noop OK\n");
}

/* Build a minimal IR carrying a single last-user text block (heap-owned so
 * aimee_request_free reclaims everything). */
static void mk_user_ir(aimee_request_t *ir, const char *user_text)
{
   fixture_session_start = 1;
   fixture_query = user_text;
   fixture_remove_tools = "[]";
   memset(ir, 0, sizeof *ir);
   ir->messages = calloc(1, sizeof *ir->messages);
   ir->n_messages = 1;
   ir->messages[0].role = strdup("user");
   ir->messages[0].blocks = calloc(1, sizeof *ir->messages[0].blocks);
   ir->messages[0].n_blocks = 1;
   ir->messages[0].blocks[0].type = AIMEE_BLK_TEXT;
   ir->messages[0].blocks[0].text = strdup(user_text);
}

static void mk_assistant_turn(aimee_request_t *ir)
{
   fixture_session_start = 0;
   ir->messages = realloc(ir->messages, (size_t)(ir->n_messages + 1) * sizeof *ir->messages);
   aimee_message_t *m = &ir->messages[ir->n_messages];
   memset(m, 0, sizeof *m);
   m->role = strdup("assistant");
   ir->n_messages += 1;
}

/* IR seam: first-turn code-owned guidance and retrieved evidence are distinct
 * system blocks with distinct authority metadata. */
static void test_ir_stage_appends_system_block(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   int rc = apply_context(&ir, NULL);
   assert(rc == 1);          /* changed typed fields -> runner marks ir->mutated */
   assert(ir.n_system == 2); /* guidance + evidence */
   assert(ir.system[0].type == AIMEE_BLK_TEXT);
   assert(ir.system[0].cache_control == NULL); /* trailing block stays uncached */
   assert(ir.system[0].context.origin == AIMEE_CTX_ORIGIN_PLATFORM);
   assert(ir.system[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
   assert(ir.system[1].context.origin == AIMEE_CTX_ORIGIN_RETRIEVAL);
   assert(ir.system[1].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(direct && ir.system[1].text && strcmp(ir.system[1].text, direct) == 0);
   assert(strstr(ir.system[0].text, "explore-with: ") != NULL);
   free(direct);
   aimee_request_free(&ir);
   printf("ir_stage_appends_system_block OK\n");
}

/* The query must be the USER's, not whatever else has been prepended to their
 * message by the time this stage runs.
 *
 * aimee_ir_apply_request_stages() inserts the persona onto the first user
 * message BEFORE the stage list runs, so reading the message here recalls
 * against the persona text. On the box that turned a question which recalls one
 * row into one that recalls none: the block assembled empty and
 * ingress_preinject_build returned NULL, killing pre-injection on the opening
 * turn of every session with no error logged anywhere. The caller now hands the
 * pristine query through `ud`. */
static void test_ir_stage_prefers_supplied_query(void)
{
   aimee_request_t ir;
   /* The message as it looks AFTER a persona prepend: the real question is in
    * there, buried, exactly as the stage would otherwise read it. */
   mk_user_ir(&ir, "<aimee-persona>lots of persona guidance here</aimee-persona> deploy matrix");
   fixture_query = "deploy matrix";
   assert(apply_context(&ir, (void *)"deploy matrix") == 1);

   /* The envelope must be the one the CLEAN query produces. */
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(direct && ir.n_system == 2 && ir.system[1].text &&
          strcmp(ir.system[1].text, direct) == 0);
   free(direct);
   aimee_request_free(&ir);

   /* And a NULL/empty ud still falls back to the message, so callers that supply
    * nothing behave exactly as before. */
   aimee_request_t ir2;
   mk_user_ir(&ir2, "deploy matrix");
   assert(apply_context(&ir2, NULL) == 1);
   assert(ir2.n_system == 2);
   aimee_request_free(&ir2);
   printf("ir_stage_prefers_supplied_query OK\n");
}

/* Mid-session with empty recall: nothing to say, so nothing is injected. The
 * guidance already shipped on the opening turn and is not repeated per turn. */
static void test_ir_stage_no_recall_midsession_noop(void)
{
   g_no_recall = 1;
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   mk_assistant_turn(&ir); /* the model has spoken -> not a session start */
   int rc = apply_context(&ir, NULL);
   assert(rc == 0);
   assert(ir.n_system == 0 && ir.system == NULL);
   aimee_request_free(&ir);
   g_no_recall = 0;
   printf("ir_stage_no_recall_midsession_noop OK\n");
}

/* SESSION START WITH EMPTY RECALL still ships the guidance. This is the whole
 * point: the standing policy used to ride inside the retrieval envelope, so on a
 * repository aimee had never indexed the agent was told nothing and reached for
 * shell. Measured on the box -- a gateway cell made zero aimee calls and the model
 * answered "PREINJECT ABSENT". Guidance does not depend on retrieval. */
static void test_ir_stage_session_start_guidance_without_recall(void)
{
   g_no_recall = 1;
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   int rc = apply_context(&ir, NULL);
   assert(rc == 1);
   assert(ir.n_system == 1 && ir.system[0].text);
   assert(strstr(ir.system[0].text, "explore-with: ") != NULL);
   assert(strstr(ir.system[0].text, "fix-scope: ") != NULL);
   aimee_request_free(&ir);
   g_no_recall = 0;
   printf("ir_stage_session_start_guidance_without_recall OK\n");
}

/* Tool-list fixtures for the first-turn shell block. */
static void mk_tool(aimee_request_t *ir, const char *name)
{
   ir->tools = realloc(ir->tools, (size_t)(ir->n_tools + 1) * sizeof *ir->tools);
   memset(&ir->tools[ir->n_tools], 0, sizeof ir->tools[0]);
   ir->tools[ir->n_tools].name = strdup(name);
   ir->n_tools += 1;
}

static int has_tool(const aimee_request_t *ir, const char *name)
{
   for (int i = 0; i < ir->n_tools; i++)
      if (ir->tools[i].name && strcmp(ir->tools[i].name, name) == 0)
         return 1;
   return 0;
}

/* The opening turn is not offered a shell. Naming the tools in the guidance was
 * measured NOT to be enough on its own -- with the guidance provably delivered, a
 * gateway cell still made zero aimee calls and grepped its way through -- so the
 * first look at a tree has to go through aimee. */
static void test_first_turn_withholds_shell(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "fix the cache");
   mk_tool(&ir, "exec_command");
   mk_tool(&ir, "apply_patch");
   mk_tool(&ir, "mcp__aimee__find_symbol");
   fixture_remove_tools = "[0]";
   assert(apply_tools(&ir, NULL) == 1);
   assert(!has_tool(&ir, "exec_command"));           /* the shell is withheld */
   assert(has_tool(&ir, "apply_patch"));             /* editing is untouched */
   assert(has_tool(&ir, "mcp__aimee__find_symbol")); /* aimee's tools remain */
   assert(ir.n_tools == 2);
   aimee_request_free(&ir);
   printf("first_turn_withholds_shell OK\n");
}

/* From the second turn the shell is back, unconditionally. This redirects the
 * FIRST look at a tree; it does not take the shell away. */
static void test_shell_returns_after_first_turn(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "fix the cache");
   mk_assistant_turn(&ir);
   mk_tool(&ir, "exec_command");
   assert(apply_tools(&ir, NULL) == 0);
   assert(has_tool(&ir, "exec_command"));
   aimee_request_free(&ir);
   printf("shell_returns_after_first_turn OK\n");
}

/* Not repeated once the model has spoken: one injection per session, not per turn. */
static void test_ir_stage_guidance_not_repeated_midsession(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   mk_assistant_turn(&ir);
   int rc = apply_context(&ir, NULL);
   assert(rc == 1); /* recall still injects its envelope */
   assert(ir.n_system == 1 && ir.system[0].text);
   assert(strstr(ir.system[0].text, "explore-with: ") == NULL);
   /* and the envelope is byte-identical to the bare build -- no wrapper, no drift */
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(direct && strcmp(ir.system[0].text, direct) == 0);
   free(direct);
   aimee_request_free(&ir);
   printf("ir_stage_guidance_not_repeated_midsession OK\n");
}

/* PERSONA PLACEMENT on the IR seam -- the DEFAULT path, and the half that was
 * untested: the flat legacy entry point had coverage, this did not.
 *
 * Two independent guards stop a second delivery. The marker catches a caller
 * that echoes our mutated first turn back; the assistant-turn scan catches the
 * caller that does NOT, which is the case a marker check alone would miss and
 * would re-personify mid-conversation. */
static void test_persona_prepends_first_user_message_once(void)
{
   static const char persona[] =
       "<aimee-persona schema=\"1\" name=\"user-edited\">\ncustom\n</aimee-persona>\n";
   aimee_request_t ir;
   mk_user_ir(&ir, "fix the cache");
   assert(aimee_ir_prepend_persona_instructions(&ir, (void *)persona) == 1);
   assert(ir.messages[0].n_blocks == 2);
   assert(strcmp(ir.messages[0].blocks[0].text, persona) == 0);
   assert(strcmp(ir.messages[0].blocks[1].text, "fix the cache") == 0);
   /* Marker present -> terminal. */
   assert(aimee_ir_prepend_persona_instructions(&ir, (void *)persona) == 0);
   assert(ir.messages[0].n_blocks == 2);
   aimee_request_free(&ir);

   /* A later request can carry the original user text WITHOUT the marker. Prior
    * assistant history is on its own sufficient to prevent a second prefix. */
   mk_user_ir(&ir, "fix the cache");
   mk_assistant_turn(&ir);
   assert(aimee_ir_prepend_persona_instructions(&ir, (void *)persona) == 0);
   assert(ir.messages[0].n_blocks == 1);
   assert(strcmp(ir.messages[0].blocks[0].text, "fix the cache") == 0);
   aimee_request_free(&ir);
   printf("persona_prepends_first_user_message_once OK\n");
}

static void test_tool_patch_validation(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "fix the cache");
   mk_tool(&ir, "exec_command");
   mk_tool(&ir, "apply_patch");
   const char *invalid[] = {
       "[1,0,0]", "[0,1]", "[1,-1]", "[1,0.5]", "[1,999999999999999999999999999999]", "null"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      fixture_remove_tools = invalid[i];
      assert(apply_tools(&ir, NULL) == 0);
      assert(ir.n_tools == 2 && has_tool(&ir, "exec_command") && has_tool(&ir, "apply_patch"));
   }
   fixture_remove_tools = "[0]";
   fixture_transport_result = -1;
   assert(apply_tools(&ir, NULL) == 0 && ir.n_tools == 2);
   fixture_transport_result = 1;
   fixture_remove_tools = "[1,0]";
   assert(apply_tools(&ir, NULL) == 1 && ir.n_tools == 0);
   aimee_request_free(&ir);
}

static void test_gate_reply_and_audit(void)
{
   fixture_gate =
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"invoke\",\"binding\":\"audit\",\"args\":{"
       "\"role\":\"RecallGateEnforcedSkip/"
       "acknowledgement\",\"query_fingerprint\":\"opaque-digest\"}}],\"result\":\"evidence\"}";
   assert(render_text("private query") == NULL && fixture_audits == 1);
   fixture_gate = "{\"status\":\"error\",\"steps\":[]}";
   assert(render_text("deploy matrix") == NULL);
   fixture_gate = NULL;
}

static void test_invalid_plans_have_no_effects(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   mk_tool(&ir, "exec_command");
   int audits = fixture_audits;
   const char *invalid[] = {
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"invoke\",\"binding\":\"audit\",\"args\":{"
       "\"role\":\"RecallGateEnforcedSkip/"
       "acknowledgement\",\"query_fingerprint\":\"opaque-digest\"}},{\"kind\":\"unknown\"}]}",
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"remove_tools\",\"indices\":[0]},{\"kind\":"
       "\"invoke\",\"binding\":\"undeclared\",\"args\":{}}]}",
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"append_context\",\"resource\":\"guidance\","
       "\"context\":{\"origin\":\"retrieval\",\"authority\":\"task_instruction\",\"trust\":"
       "\"verified\",\"sensitivity\":\"internal\",\"model_visible\":true}}]}",
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"invoke\",\"binding\":\"audit\",\"args\":[],"
       "\"output\":\"\"}]}",
       "{\"status\":\"ok\",\"steps\":null}"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      fixture_gate = invalid[i];
      assert(apply_context(&ir, NULL) == 0 && ir.n_system == 0 && ir.n_tools == 1);
      assert(fixture_audits == audits);
   }
   fixture_gate = NULL;
   aimee_request_free(&ir);
}
static const char *opaque_payload;
static cJSON *payload_binding(const cJSON *args, void *context)
{
   (void)args;
   assert(context == (void *)&opaque_payload);
   return cJSON_CreateString(opaque_payload);
}
static cJSON *clock_binding(const cJSON *args, void *context)
{
   (void)args;
   (void)context;
   return cJSON_CreateString("18446744073709551615");
}
static void test_generic_bindings_full_text_and_epoch(void)
{
   char payload[12001];
   memset(payload, 'x', sizeof(payload) - 1);
   payload[sizeof(payload) - 1] = 0;
   opaque_payload = payload;
   const aimee_ir_plan_binding_t bindings[] = {
       {"payload", payload_binding}, {"clock", clock_binding}, {NULL, NULL}};
   aimee_ir_module_plan_t config = {.method = "memory.runtime",
                                    .operation = "gateway-plan",
                                    .phase = "text",
                                    .bindings = bindings,
                                    .context = &opaque_payload};
   fixture_gate = "{\"status\":\"ok\",\"steps\":[{\"kind\":\"invoke\",\"binding\":\"payload\","
                  "\"args\":{},\"output\":\"value\"}],\"result\":\"value\"}";
   char *text = aimee_ir_module_plan_text(&config);
   assert(text && strcmp(text, payload) == 0);
   free(text);
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   config.phase = "context";
   fixture_gate =
       "{\"status\":\"ok\",\"steps\":[{\"kind\":\"invoke\",\"binding\":\"payload\",\"args\":{},"
       "\"output\":\"value\"},{\"kind\":\"invoke\",\"binding\":\"clock\",\"args\":{},\"output\":"
       "\"clock\"},{\"kind\":\"append_context\",\"output\":\"value\",\"epoch_output\":\"clock\","
       "\"context\":{\"origin\":\"retrieval\",\"authority\":\"evidence\",\"trust\":\"unverified\","
       "\"sensitivity\":\"internal\",\"model_visible\":true,\"revision_domain\":\"domain\","
       "\"revision_scope\":\"scope\"}}]}";
   assert(aimee_ir_stage_module_plan(&ir, &config) == 1);
   assert(ir.n_system == 1 && strcmp(ir.system[0].text, payload) == 0);
   assert(ir.system[0].context.revision_epoch == UINT64_MAX);
   assert(strcmp(ir.system[0].context.revision_domain, "domain") == 0);
   fixture_gate = NULL;
   aimee_request_free(&ir);
}

int main(void)
{
   printf("test_ir_module_plan:\n");
   test_system_prompt_raw_env();
   test_gate_reply_and_audit();
   test_disabled_noop();
   test_ir_stage_appends_system_block();
   const char *const provided[] = {"guidance", NULL};
   fixture_provided_resources = provided;
   test_ir_stage_appends_system_block();
   fixture_provided_resources = NULL;
   test_ir_stage_prefers_supplied_query();
   test_ir_stage_no_recall_midsession_noop();
   test_ir_stage_session_start_guidance_without_recall();
   test_ir_stage_guidance_not_repeated_midsession();
   test_first_turn_withholds_shell();
   test_tool_patch_validation();
   test_invalid_plans_have_no_effects();
   test_generic_bindings_full_text_and_epoch();
   test_shell_returns_after_first_turn();
   test_persona_prepends_first_user_message_once();
   printf("all IR module plan tests passed\n");
   return 0;
}

/* test_gw_stage_memory.c: unit tests for the ONE shared memory-injection stage
 * (universal-gateway P3, gw_stage_memory.c). The point of P3 is consolidation
 * WITHOUT changing rendered bytes, so these are byte-identity tests across the
 * three render targets:
 *   - OPENAI_SYSTEM_PROMPT (legacy text handlers): the RAW envelope, byte-for-
 *     byte what ingress_preinject_build(query, 0) returned inline before P3 —
 *     crucially NO trailing "\n\n".
 *   - OPENAI_INSTRUCTIONS (/v1/responses): env + "\n\n" + prior, via apply().
 *   - ANTHROPIC_MESSAGES: dispatch + the parity gate (the Anthropic applier
 *     itself, messages_apply_preinject, lives in anthropic_http.c and is covered
 *     by the anthropic tests; here it is stubbed to assert dispatch/gating).
 *
 * The kb client / config are stubbed (as in test_ingress_preinject.c) so
 * ingress_preinject_build produces a deterministic envelope; platform_random is
 * FIXED here so two build() calls render identically and can be compared. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/memory/gw_stage_memory.h"
#include "ingress_preinject.h"
#include "cJSON.h"
#include "config.h"
#include "kb_client.h"

/* When set, the recall stubs return nothing so ingress_preinject_build → NULL
 * (the "pre-injection off / recall empty" path). */
static int g_no_recall = 0;
static int g_test_placement = 0; /* drives ingress_cache_placement_enabled in config_load stub */

/* --- stubs: make ingress_preinject_build deterministic without the kb graph --- */
char *kb_client_memory_context_block(const char *query, const char *block_type, int limit)
{
   (void)query;
   (void)block_type;
   (void)limit;
   return NULL;
}
char *kb_client_memory_facts(const char *query)
{
   (void)query;
   return NULL;
}

/* Typed-facts gate stub (typed_facts feature added this call to ingress_preinject.c;
 * the test link needs the symbol). Off -> the builder's facts path stays inert. */
int kb_client_typed_facts_enabled(void)
{
   return 0;
}
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
int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)limit;
   if (g_no_recall || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   out[0].memory.id = 101;
   snprintf(out[0].memory.tier, sizeof(out[0].memory.tier), "L2");
   snprintf(out[0].memory.kind, sizeof(out[0].memory.kind), "fact");
   snprintf(out[0].memory.key, sizeof(out[0].memory.key), "deploy path");
   snprintf(out[0].memory.headline, sizeof(out[0].memory.headline), "Use the deploy matrix.");
   out[0].parts.total = 0.88;
   return 1;
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
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->ingress_preinject_enabled = 1;
      cfg->ingress_preinject_assembly_budget = 1200;
      cfg->ingress_cache_placement_enabled = g_test_placement;
   }
   return 0;
}

/* Accessor stubs: the production seam moved from config_load to per-field
 * accessors. Values match what this file's config_load stub produced, so the
 * assertions below are unchanged. */
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
   return "/tmp/aimee-test";
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

static int test_confidence_provider(double score, const char **confidence)
{
   if (!confidence)
      return -1;
   *confidence = score >= 0.66 ? "high" : score >= 0.33 ? "medium" : "low";
   return 0;
}

/* OPENAI_SYSTEM_PROMPT: gw_memory_system_prompt(q) == ingress_preinject_build(q,0)
 * byte-for-byte (the raw env, no trailing "\n\n"). */
static void test_system_prompt_raw_env(void)
{
   char *sys = gw_memory_system_prompt("deploy matrix");
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
   assert(gw_memory_system_prompt("deploy matrix") == NULL);
   g_no_recall = 0;
   printf("disabled_noop OK\n");
}

/* Build a minimal IR carrying a single last-user text block (heap-owned so
 * aimee_request_free reclaims everything). */
static void mk_user_ir(aimee_request_t *ir, const char *user_text)
{
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
   ir->messages = realloc(ir->messages, (size_t)(ir->n_messages + 1) * sizeof *ir->messages);
   aimee_message_t *m = &ir->messages[ir->n_messages];
   memset(m, 0, sizeof *m);
   m->role = strdup("assistant");
   ir->n_messages += 1;
}

/* IR seam (P4 port): ir_stage_memory appends the envelope as a trailing system TEXT
 * block, derives the query from the last user message, and reports the mutation. The
 * appended env is byte-identical to the legacy ingress_preinject_build(query, 0). */
static void test_ir_stage_appends_system_block(void)
{
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   int rc = ir_stage_memory(&ir, NULL);
   assert(rc == 1);          /* changed typed fields -> runner marks ir->mutated */
   assert(ir.n_system == 1); /* exactly one trailing block appended */
   assert(ir.system[0].type == AIMEE_BLK_TEXT);
   assert(ir.system[0].cache_control == NULL); /* trailing block stays uncached */
   /* Session start, so the block is the guidance FOLLOWED BY this turn's recall
    * envelope. The envelope itself is unchanged -- assert it is carried verbatim
    * inside; byte-identity with the bare envelope is asserted mid-session, where
    * the guidance is not repeated. */
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(direct && ir.system[0].text && strstr(ir.system[0].text, direct) != NULL);
   assert(strstr(ir.system[0].text, "explore-with: ") != NULL);
   free(direct);
   aimee_request_free(&ir);
   printf("ir_stage_appends_system_block OK\n");
}

/* Mid-session with empty recall: nothing to say, so nothing is injected. The
 * guidance already shipped on the opening turn and is not repeated per turn. */
static void test_ir_stage_no_recall_midsession_noop(void)
{
   g_no_recall = 1;
   aimee_request_t ir;
   mk_user_ir(&ir, "deploy matrix");
   mk_assistant_turn(&ir); /* the model has spoken -> not a session start */
   int rc = ir_stage_memory(&ir, NULL);
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
   int rc = ir_stage_memory(&ir, NULL);
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
   assert(ir_stage_first_turn_shell_block(&ir, NULL) == 1);
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
   assert(ir_stage_first_turn_shell_block(&ir, NULL) == 0);
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
   int rc = ir_stage_memory(&ir, NULL);
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

int main(void)
{
   printf("test_gw_stage_memory:\n");
   ingress_preinject_register_confidence_provider(test_confidence_provider);
   test_system_prompt_raw_env();
   test_disabled_noop();
   test_ir_stage_appends_system_block();
   test_ir_stage_no_recall_midsession_noop();
   test_ir_stage_session_start_guidance_without_recall();
   test_ir_stage_guidance_not_repeated_midsession();
   test_first_turn_withholds_shell();
   test_shell_returns_after_first_turn();
   printf("all gw_stage_memory tests passed\n");
   return 0;
}

/* gw_stage_memory.c: memory/context injection on the IR.
 *
 * ir_stage_memory is THE injection point. Every protocol converges on the IR, so
 * the CLI, MCP and the gateway get identical behaviour from one function --
 * which is the whole reason the per-wire stage that used to live here is gone.
 *
 * What was deleted and why: gw_stage_memory() carried three render targets
 * (Anthropic messages / responses instructions / legacy system prompt) that had
 * to be kept byte-identical to each other by hand. Both structured arms were
 * ported to the IR seam and their slot-catalog entries removed, leaving the
 * function reachable only from a helper that built a throwaway cJSON object just
 * to call it. Three hand-synchronised copies of one policy is how the guidance
 * text itself drifted; one path cannot drift.
 *
 * gw_memory_system_prompt stays only until the four plain-chat handlers move onto
 * the IR too -- it is now a direct call, not a stage. */
#include "gw_stage_memory.h"
#include "module_stage_adapters.h"
#include "aimee_session_guidance.h"
#include "ingress_preinject.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/core/turn_integrity.h>
#include "cJSON.h"
#include "log.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <strings.h> /* strcasecmp */
#include <string.h>

/* Turn-level recall gate; defined below next to its mode/classifier helpers. */
static int recall_gate_skip_turn(const char *query);

static int ir_append_context_block(aimee_request_t *ir, char *owned_text,
                                   aimee_context_origin_t origin,
                                   aimee_context_authority_t authority, aimee_context_trust_t trust,
                                   const char *revision_domain, const char *revision_scope,
                                   unsigned long long revision_epoch)
{
   if (!ir || !owned_text)
      return 0;
   aimee_block_t *grown = realloc(ir->system, (size_t)(ir->n_system + 1) * sizeof *grown);
   if (!grown)
      return 0;
   ir->system = grown;
   aimee_block_t *block = &ir->system[ir->n_system];
   memset(block, 0, sizeof *block);
   block->type = AIMEE_BLK_TEXT;
   block->text = owned_text;
   aimee_ir_block_set_context(block, origin, authority, trust, AIMEE_CTX_SENS_INTERNAL, 1,
                              revision_domain, revision_scope, revision_epoch);
   ir->n_system++;
   return 1;
}

/* Recall-query buffer for the IR transform. The query only feeds semantic KB
 * recall, so bounding an over-long last-user message here is acceptable (it does
 * not change what the model receives — only which memories are retrieved). */
#define IR_MEMORY_QUERY_MAX 16384

int ir_session_start(const aimee_request_t *ir)
{
   /* No assistant turn yet == the model has not spoken == start of session.
    *
    * NOT n_messages == 1. A real client does not open with a single message:
    * Codex prepends environment/instructions items, so the opening turn arrives
    * with several. Counting messages was tried on the box and never fired -- the
    * probe still answered "PREINJECT ABSENT" with the transform live and reached.
    * What is invariant is that nothing the ASSISTANT said can be in the history
    * before the assistant has said anything.
    *
    * This also covers COMPACTION, the other moment the guidance is needed: a
    * compacted history is a carried-over summary with no assistant turn in it, so
    * the rule fires again exactly when compaction discarded the first copy. */
   if (!ir)
      return 0;
   for (int i = 0; i < ir->n_messages; i++)
   {
      const char *role = ir->messages[i].role;
      if (role && strcmp(role, "assistant") == 0)
         return 0;
   }
   return 1;
}

/* Codex's own shell tools. Everything else it carries -- apply_patch, update_plan
 * -- is left alone: this is about how the agent LOOKS at code, not how it edits
 * or plans. */
static int ir_is_codex_shell_tool(const char *name)
{
   if (!name)
      return 0;
   static const char *const SHELL[] = {"exec_command", "local_shell", "shell",
                                       "bash",         "run_command", "container.exec"};
   for (size_t i = 0; i < sizeof SHELL / sizeof SHELL[0]; i++)
      if (strcmp(name, SHELL[i]) == 0)
         return 1;
   return 0;
}

int ir_stage_first_turn_shell_block(aimee_request_t *ir, void *ud)
{
   (void)ud;
   /* WITHHOLD THE SHELL FOR THE OPENING TURN, ONCE PER SESSION.
    *
    * Telling the agent which tool to use does not work on its own. Measured on
    * CT 403 with the guidance demonstrably delivered -- the model quoted it back
    * verbatim on request -- a gateway cell still made ZERO aimee calls, by MCP or
    * CLI, and did all eight of its steps with find/cat/sed/grep. Advice loses to a
    * shell the model already knows how to drive.
    *
    * So the opening turn is not offered one. The agent still has aimee's
    * symbol-scoped tools and the guidance naming which shell reflex each replaces,
    * so the turn it would have spent grepping is spent through aimee instead. From
    * the second turn the shell is back, unconditionally: this redirects the FIRST
    * look at a tree, it does not take the shell away.
    *
    * Deliberately not a config toggle, for the same reason the guidance is not
    * one: an agent that never reaches aimee's tools is not using aimee. */
   if (!ir || !ir_session_start(ir))
      return 0;
   int removed = 0;
   for (int i = 0; i < ir->n_tools;)
   {
      if (!ir_is_codex_shell_tool(ir->tools[i].name))
      {
         i++;
         continue;
      }
      free(ir->tools[i].name);
      free(ir->tools[i].description);
      cJSON_Delete(ir->tools[i].schema);
      free(ir->tools[i].cache_control);
      cJSON_Delete(ir->tools[i].raw);
      for (int j = i; j + 1 < ir->n_tools; j++)
         ir->tools[j] = ir->tools[j + 1];
      ir->n_tools -= 1;
      removed = 1;
   }
   return removed;
}

int ir_stage_memory(aimee_request_t *ir, void *ud)
{
   if (!ir)
      return 0;

   /* `ud`, when the caller supplies it, is the user's query as it arrived --
    * captured before the persona was prepended to the same message. Reading the
    * message here instead would recall against the persona text: it is inserted
    * onto the first user message before this stage runs, so on the opening turn
    * of every session the query became thousands of characters of persona and
    * matched nothing. NULL keeps the old behaviour for callers that pass none. */
   const char *supplied_query = (const char *)ud;

   /* No assistant turn yet == the model has not spoken == start of session.
    *
    * NOT n_messages == 1. A real client does not open with a single message:
    * Codex prepends environment/instructions items, so the opening turn arrives
    * with several. Counting messages was tried on the box and never fired --
    * the probe still answered "PREINJECT ABSENT" with the transform live and
    * reached. What is invariant is that nothing the ASSISTANT said can be in the
    * history before the assistant has said anything.
    *
    * This still covers compaction, which is the other moment guidance is needed:
    * a compacted history is a carried-over summary with no assistant turn in it,
    * so the rule fires again exactly when compaction discarded the first copy. */
   int session_start = ir_session_start(ir);

   char *env = NULL;
   if (supplied_query && supplied_query[0])
   {
      env =
          recall_gate_skip_turn(supplied_query) ? NULL : ingress_preinject_build(supplied_query, 0);
   }
   else
   {
      char *query = malloc(IR_MEMORY_QUERY_MAX);
      if (!query)
         return 0;
      size_t qn = aimee_ir_last_user_text(ir, query, IR_MEMORY_QUERY_MAX);
      env = (qn > 0 && !recall_gate_skip_turn(query)) ? ingress_preinject_build(query, 0) : NULL;
      free(query);
   }
   if (!env && !session_start)
      return 0; /* nothing to say this turn: byte-identical no-op */

   int changed = 0;
   if (session_start)
   {
      /* Guidance is code-owned task instruction. Keep it in a distinct block
       * from recalled evidence so provider rendering cannot erase the authority
       * boundary inside the canonical IR. */
      char *guidance = strdup(AIMEE_GUIDANCE_BLOCK);
      if (guidance && ir_append_context_block(ir, guidance, AIMEE_CTX_ORIGIN_PLATFORM,
                                              AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                              AIMEE_CTX_TRUST_VERIFIED, NULL, NULL, 0))
         changed = 1;
      else
         free(guidance);
   }
   if (env)
   {
      unsigned long long epoch = ti_knowledge_epoch_current("knowledge", "global");
      if (ir_append_context_block(ir, env, AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                                  AIMEE_CTX_TRUST_UNVERIFIED, "knowledge", "global", epoch))
         changed = 1;
      else
         free(env);
   }
   return changed; /* changed typed fields -> runner sets ir->mutated */
}

/* Place the caller-resolved persona payload on the first user message. */
int ir_stage_persona_instructions(aimee_request_t *ir, void *ud)
{
   return aimee_ir_prepend_persona_instructions(ir, (const char *)ud);
}

char *gw_memory_system_prompt(const char *query)
{
   /* The four plain-chat handlers are the last callers that are not on the IR.
    * This used to build a throwaway cJSON object, push it through
    * gw_stage_memory's GW_MEM_OPENAI_SYSTEM_PROMPT arm, then read the string back
    * out -- ceremony around one call, and the last thing keeping that stage
    * alive. NULL (not "") when nothing was injected, exactly as before. */
   return recall_gate_skip_turn(query) ? NULL : ingress_preinject_build(query, 0);
}

/* Turn-level recall policy lives in the Go memory module. This file only
 * translates the gateway call to the shared event-bus data stage and retains
 * process-local transport telemetry for the legacy metrics endpoint. */

static _Atomic unsigned long long g_recall_gate_predicted_skip = 0;
static _Atomic unsigned long long g_recall_gate_predicted_retrieve = 0;
static _Atomic unsigned long long g_recall_gate_wrongly_skipped = 0;
static _Atomic unsigned long long g_recall_gate_wrongly_performed = 0;

/* The gateway can run in lean binaries that do not link the learning/evidence
 * writer. The DB2-disabled server build also has to use its existing KB client
 * retrieval-event seam rather than pulling a direct DB2 writer across the
 * process boundary. When the local writer is present, gate decisions use the
 * same retrieval_event artifact type as ordinary recall; absence or store
 * failure never changes the fail-open decision. */
extern int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                                   const int64_t *surfaced_ids, int n_surfaced,
                                                   char *id_out, int id_out_len)
    __attribute__((weak));

static int recall_gate_call(const char *query, int *predicted_skip, int *enforced,
                            const char **reason_out)
{
   if (predicted_skip)
      *predicted_skip = 0;
   if (enforced)
      *enforced = 0;
   if (reason_out)
      *reason_out = NULL;
   if (!query)
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return -1;
   cJSON_AddStringToObject(request, "operation", "recall-gate");
   cJSON_AddStringToObject(request, "query", query);
   cJSON *response = server_module_memory_data(request);
   cJSON_Delete(request);
   if (!response)
      return -1;
   cJSON *skip = cJSON_GetObjectItemCaseSensitive(response, "skip");
   cJSON *enforce = cJSON_GetObjectItemCaseSensitive(response, "enforced");
   cJSON *reason = cJSON_GetObjectItemCaseSensitive(response, "reason");
   if (predicted_skip)
      *predicted_skip = cJSON_IsTrue(skip);
   if (enforced)
      *enforced = cJSON_IsTrue(enforce);
   if (reason_out && cJSON_IsString(reason) && strcmp(reason->valuestring, "acknowledgement") == 0)
      *reason_out = "acknowledgement";
   cJSON_Delete(response);
   return 0;
}

int gw_stage_memory_recall_gate_should_skip(const char *query, const char **reason_out)
{
   int predicted = 0;
   return recall_gate_call(query, &predicted, NULL, reason_out) == 0 ? predicted : 0;
}

void gw_stage_memory_recall_gate_record_outcome(int gate_predicted_skip, int retrieval_was_needed)
{
   if (gate_predicted_skip && retrieval_was_needed)
      atomic_fetch_add_explicit(&g_recall_gate_wrongly_skipped, 1, memory_order_relaxed);
   else if (!gate_predicted_skip && !retrieval_was_needed)
      atomic_fetch_add_explicit(&g_recall_gate_wrongly_performed, 1, memory_order_relaxed);
}

void gw_stage_memory_recall_gate_metrics(gw_memory_recall_gate_metrics_t *out)
{
   if (!out)
      return;
   out->predicted_skip = atomic_load_explicit(&g_recall_gate_predicted_skip, memory_order_relaxed);
   out->predicted_retrieve =
       atomic_load_explicit(&g_recall_gate_predicted_retrieve, memory_order_relaxed);
   out->wrongly_skipped =
       atomic_load_explicit(&g_recall_gate_wrongly_skipped, memory_order_relaxed);
   out->wrongly_performed =
       atomic_load_explicit(&g_recall_gate_wrongly_performed, memory_order_relaxed);
}

/* Returns 1 when the caller should skip recall for this turn. Always logs the
 * decision when the gate fires, in both observe and enforce mode. */
static int recall_gate_skip_turn(const char *query)
{
   int predicted = 0, enforced = 0;
   const char *reason = NULL;
   if (recall_gate_call(query, &predicted, &enforced, &reason) != 0 || !predicted)
   {
      atomic_fetch_add_explicit(&g_recall_gate_predicted_retrieve, 1, memory_order_relaxed);
      return 0;
   }
   atomic_fetch_add_explicit(&g_recall_gate_predicted_skip, 1, memory_order_relaxed);
   LOG_INFO("memory", "recall gate: %s turn (reason=%s)", enforced ? "skipping" : "would skip",
            reason ? reason : "unclassified");
   if (learning_evidence_write_retrieval_event)
   {
      char role[96];
      snprintf(role, sizeof(role), "RecallGate%sSkip/%s", enforced ? "Enforced" : "Observed",
               reason ? reason : "unclassified");
      (void)learning_evidence_write_retrieval_event(query, role, NULL, 0, NULL, 0);
   }
   return enforced;
}

int gw_stage_memory_enabled(void)
{
   /* Default-ON: memory injection runs unless AIMEE_STAGE_MEMORY is an explicit
    * disable token. Full-token match (not first-byte) so "false"/"no" disable but
    * "foo"/"nope" do not. */
   const char *v = getenv("AIMEE_STAGE_MEMORY");
   if (!v || !v[0])
      return 1;
   return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
            strcasecmp(v, "no") == 0);
}

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
#include "module_commands.h"
#include "aimee_session_guidance.h"
#include "ingress_preinject.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/core/turn_integrity.h>
#include "cJSON.h"
#include "log.h"
#include <assert.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
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

static cJSON *gateway_call(cJSON *request)
{
   cJSON *response = NULL;
   int rc = request ? aimee_module_commands_dispatch_internal_timeout("memory.runtime", request,
                                                                      500, &response)
                    : -1;
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   if (rc <= 0 || !status || strcmp(status, "ok") != 0)
   {
      cJSON_Delete(response);
      return NULL;
   }
   return response;
}

static cJSON *gateway_plan(const aimee_request_t *ir)
{
   if (!ir)
      return NULL;
   cJSON *request = cJSON_CreateObject();
   cJSON *roles = cJSON_AddArrayToObject(request, "roles");
   cJSON *tools = cJSON_AddArrayToObject(request, "tools");
   if (!request || !roles || !tools ||
       !cJSON_AddStringToObject(request, "operation", "gateway-plan"))
      goto failed;
   for (int i = 0; i < ir->n_messages; i++)
      if (!cJSON_AddItemToArray(
              roles, cJSON_CreateString(ir->messages[i].role ? ir->messages[i].role : "")))
         goto failed;
   for (int i = 0; i < ir->n_tools; i++)
      if (!cJSON_AddItemToArray(tools,
                                cJSON_CreateString(ir->tools[i].name ? ir->tools[i].name : "")))
         goto failed;
   return gateway_call(request);
failed:
   cJSON_Delete(request);
   return NULL;
}

/* Codex's own shell tools. Everything else it carries -- apply_patch, update_plan
 * -- is left alone: this is about how the agent LOOKS at code, not how it edits
 * or plans. */

int ir_stage_first_turn_shell_block(aimee_request_t *ir, void *ud)
{
   (void)ud;
   cJSON *plan = gateway_plan(ir);
   int changed = aimee_ir_remove_tools(ir, cJSON_GetObjectItemCaseSensitive(plan, "remove_tools"));
   cJSON_Delete(plan);
   return changed > 0;
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
   cJSON *plan = gateway_plan(ir);
   int session_start = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "append_guidance"));
   cJSON_Delete(plan);

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

char *gw_memory_system_prompt(const char *query)
{
   /* The four plain-chat handlers are the last callers that are not on the IR.
    * This used to build a throwaway cJSON object, push it through
    * gw_stage_memory's GW_MEM_OPENAI_SYSTEM_PROMPT arm, then read the string back
    * out -- ceremony around one call, and the last thing keeping that stage
    * alive. NULL (not "") when nothing was injected, exactly as before. */
   return recall_gate_skip_turn(query) ? NULL : ingress_preinject_build(query, 0);
}

/* The Go owner records telemetry and returns the opaque audit/log payload.
 * The native host keeps its existing authenticated evidence connection. */
extern int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                                   const int64_t *surfaced_ids, int n_surfaced,
                                                   char *id_out, int id_out_len)
    __attribute__((weak));

static int recall_gate_skip_turn(const char *query)
{
   if (!query)
      return 0;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "gateway-recall") ||
       !cJSON_AddStringToObject(request, "query", query))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = gateway_call(request);
   int enforced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "enforced"));
   const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "log"));
   const char *role =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "audit_role"));
   const char *fingerprint =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "query_fingerprint"));
   if (message)
      LOG_INFO("memory", "%s", message);
   if (role && fingerprint && learning_evidence_write_retrieval_event)
      (void)learning_evidence_write_retrieval_event(fingerprint, role, NULL, 0, NULL, 0);
   cJSON_Delete(response);
   return enforced;
}

int gw_stage_memory_enabled(void)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "gateway-enabled");
   const char *value = getenv("AIMEE_STAGE_MEMORY");
   cJSON_AddStringToObject(request, "value", value ? value : "");
   cJSON *response = gateway_call(request);
   /* An unavailable module must not silently turn the stage off. */
   int enabled = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(response, "enabled"));
   cJSON_Delete(response);
   return enabled;
}

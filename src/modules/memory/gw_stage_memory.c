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
#include "aimee_session_guidance.h"
#include "ingress_preinject.h"
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <strings.h> /* strcasecmp */
#include <string.h>

/* Recall-query buffer for the IR transform. The query only feeds semantic KB
 * recall, so bounding an over-long last-user message here is acceptable (it does
 * not change what the model receives — only which memories are retrieved). */
#define IR_MEMORY_QUERY_MAX 16384

int ir_stage_memory(aimee_request_t *ir, void *ud)
{
   (void)ud; /* query comes from the IR, not per-call user data */
   if (!ir)
      return 0;

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
   int session_start = 1;
   for (int i = 0; i < ir->n_messages; i++)
   {
      const char *role = ir->messages[i].role;
      if (role && strcmp(role, "assistant") == 0)
      {
         session_start = 0;
         break;
      }
   }

   char *query = malloc(IR_MEMORY_QUERY_MAX);
   if (!query)
      return 0;
   size_t qn = aimee_ir_last_user_text(ir, query, IR_MEMORY_QUERY_MAX);
   char *env = (qn > 0) ? ingress_preinject_build(query, 0) : NULL;
   free(query);
   if (!env && !session_start)
      return 0; /* nothing to say this turn: byte-identical no-op */

   if (session_start)
   {
      /* Guidance first, then this turn's retrieval block if there is one. */
      size_t n = sizeof(AIMEE_GUIDANCE_BLOCK) + (env ? strlen(env) + 1 : 0);
      char *both = malloc(n);
      if (!both)
      {
         free(env);
         return 0;
      }
      snprintf(both, n, "%s%s%s", AIMEE_GUIDANCE_BLOCK, env ? "\n" : "", env ? env : "");
      free(env);
      env = both;
   }

   /* Append the envelope as a trailing system TEXT block. Grow the ordered block
    * array by one; the new block owns `env` (freed by aimee_request_free) and
    * carries no cache_control / raw sidecar so the backend serializes it from the
    * typed field per wire — collapsing the old three per-wire arms into one. */
   aimee_block_t *grown = realloc(ir->system, (size_t)(ir->n_system + 1) * sizeof *grown);
   if (!grown)
   {
      free(env);
      return 0;
   }
   ir->system = grown;
   aimee_block_t *b = &ir->system[ir->n_system];
   memset(b, 0, sizeof *b);
   b->type = AIMEE_BLK_TEXT;
   b->text = env;
   ir->n_system += 1;
   return 1; /* changed typed fields -> runner sets ir->mutated */
}

char *gw_memory_system_prompt(const char *query)
{
   /* The four plain-chat handlers are the last callers that are not on the IR.
    * This used to build a throwaway cJSON object, push it through
    * gw_stage_memory's GW_MEM_OPENAI_SYSTEM_PROMPT arm, then read the string back
    * out -- ceremony around one call, and the last thing keeping that stage
    * alive. NULL (not "") when nothing was injected, exactly as before. */
   return ingress_preinject_build(query, 0);
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

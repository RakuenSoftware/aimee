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
   int session_start = ir_session_start(ir);

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

/* aimee_ir_rescue.c -- see aimee_ir_rescue.h. */
#include "aimee.h" /* MAX_PATH_LEN et al: agent_types.h depends on it */

#include "aimee_ir_rescue.h"

#include <stdlib.h>
#include <string.h>

#include "agent_protocol.h"
#include "aimee_ir_metrics.h"
#include "cJSON.h"
#include "delegate_xml_fallback.h"

static void free_parsed(parsed_response_t *pr)
{
   for (int i = 0; i < pr->call_count; i++)
      free(pr->calls[i].arguments);
   free(pr->content);
   if (pr->assistant_message)
      cJSON_Delete(pr->assistant_message);
}

/* A tool call's arguments arrive as a JSON string. Keep the parsed object when it
 * parses; fall back to an empty object rather than dropping the call, so a model
 * that emits malformed args still gets dispatched and fails loudly downstream
 * instead of silently vanishing here. */
static cJSON *args_to_json(const char *arguments)
{
   cJSON *j = arguments ? cJSON_Parse(arguments) : NULL;
   if (j && cJSON_IsObject(j))
      return j;
   if (j)
      cJSON_Delete(j);
   return cJSON_CreateObject();
}

int aimee_ir_rescue_tool_calls(aimee_response_t *r, int allow_json)
{
   if (!r || !r->content || r->n_content <= 0)
      return 0;

   /* Native tool calling already worked -- do not double-dispatch. */
   if (aimee_ir_response_has_tool_use(r))
      return 0;

   aimee_block_t *out = NULL;
   int n_out = 0, cap = 0, rescued = 0;

   for (int i = 0; i < r->n_content; i++)
   {
      aimee_block_t *b = &r->content[i];

      /* THINKING is never scanned: reasoning about a tool is not a call. IMAGE,
       * DOCUMENT and UNKNOWN pass through untouched. */
      parsed_response_t pr;
      int n_calls = 0;
      if (b->type == AIMEE_BLK_TEXT && b->text)
      {
         memset(&pr, 0, sizeof(pr));
         n_calls = delegate_rescue_parse_tool_calls(b->text, &pr, allow_json);
      }

      int need = n_out + 1 + (n_calls > 0 ? n_calls : 0);
      if (need > cap)
      {
         int ncap = need > cap * 2 ? need : cap * 2;
         aimee_block_t *grown = realloc(out, (size_t)ncap * sizeof(*out));
         if (!grown)
         {
            if (n_calls > 0)
               free_parsed(&pr);
            free(out);
            return 0; /* leave `r` untouched: a partial rewrite is worse than none */
         }
         out = grown;
         cap = ncap;
      }

      if (n_calls <= 0)
      {
         out[n_out++] = *b; /* move: ownership transfers to `out` */
         continue;
      }

      /* Any prose preceding the first call is real assistant text; keep it, drop the
       * block when the model emitted nothing but the call. */
      if (pr.content && pr.content[0])
      {
         memset(&out[n_out], 0, sizeof(out[n_out]));
         out[n_out].type = AIMEE_BLK_TEXT;
         out[n_out].text = strdup(pr.content);
         out[n_out].cache_control = b->cache_control ? strdup(b->cache_control) : NULL;
         n_out++;
      }

      for (int c = 0; c < pr.call_count; c++)
      {
         memset(&out[n_out], 0, sizeof(out[n_out]));
         out[n_out].type = AIMEE_BLK_TOOL_USE;
         out[n_out].tool_id = strdup(pr.calls[c].id);
         out[n_out].tool_name = strdup(pr.calls[c].name);
         out[n_out].tool_input = args_to_json(pr.calls[c].arguments);
         n_out++;
         rescued++;
      }

      aimee_block_free_contents(b); /* the source TEXT block is fully consumed */
      free_parsed(&pr);
   }

   if (!rescued)
   {
      free(out); /* blocks were moved, not copied -- `r` still owns them */
      return 0;
   }

   free(r->content);
   r->content = out;
   r->n_content = n_out;

   /* A rescued call IS a tool call: the stop reason must say so, or the caller
    * treats a tool-calling turn as a finished answer and never dispatches it. */
   r->stop_reason = AIMEE_STOP_TOOL_USE;

   aimee_ir_metric_inc(AIMEE_IR_M_RESCUE_RECOVERIES, AIMEE_WIRE_UNKNOWN);
   return rescued;
}

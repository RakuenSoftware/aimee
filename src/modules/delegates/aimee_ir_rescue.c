/* aimee_ir_rescue.c -- see aimee_ir_rescue.h. */
#include "aimee.h" /* MAX_PATH_LEN et al: agent_types.h depends on it */

#include <aimee/delegates/aimee_ir_rescue.h>

#include <stdlib.h>
#include <string.h>

#include "agent_protocol.h"
#include <aimee/ir/aimee_ir_metrics.h>
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

typedef struct
{
   /* The dialect parser does not mutate its input. It owns these parsed
    * allocations until free_parsed runs after commit or rollback. */
   parsed_response_t parsed;
   int n_calls;
} rescue_prepared_t;

static int block_is_rescue_eligible(const aimee_block_t *block)
{
   return block && block->type == AIMEE_BLK_TEXT && block->text;
}

static void free_prepared(rescue_prepared_t *prepared, int count)
{
   if (!prepared)
      return;
   for (int i = 0; i < count; i++)
      free_parsed(&prepared[i].parsed);
   free(prepared);
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

   const int source_n_content = r->n_content;

   /* Prepare every eligible block without modifying the response. Both this pass
    * and the commit pass use block_is_rescue_eligible so THINKING and non-text
    * block behavior cannot drift between them. */
   rescue_prepared_t *prepared = calloc((size_t)source_n_content, sizeof(*prepared));
   if (!prepared)
      return 0;

   int rescued = 0;
   int final_n_content = 0;

   for (int i = 0; i < source_n_content; i++)
   {
      aimee_block_t *b = &r->content[i];
      rescue_prepared_t *p = &prepared[i];

      if (block_is_rescue_eligible(b))
      {
         int found = delegate_rescue_parse_tool_calls(b->text, &p->parsed, allow_json);
         if (found > 0)
            p->n_calls = p->parsed.call_count;
      }

      if (p->n_calls <= 0)
      {
         final_n_content++;
         continue;
      }

      rescued += p->n_calls;
      final_n_content += p->n_calls;
      if (p->parsed.content && p->parsed.content[0])
         final_n_content++;
   }

   if (!rescued)
   {
      free_prepared(prepared, source_n_content);
      return 0;
   }

   /* This is the last structural allocation. If it fails, the response is still
    * byte-for-byte owned by `r`; no source block has been consumed or moved. */
   aimee_block_t *out = calloc((size_t)final_n_content, sizeof(*out));
   if (!out)
   {
      free_prepared(prepared, source_n_content);
      return 0;
   }

   int n_out = 0;
   for (int i = 0; i < source_n_content; i++)
   {
      aimee_block_t *b = &r->content[i];
      rescue_prepared_t *p = &prepared[i];

      /* Keep the same eligibility predicate in prepare and commit. Ineligible and
       * zero-call blocks transfer without deep-copying their contents. */
      if (!block_is_rescue_eligible(b) || p->n_calls <= 0)
      {
         out[n_out++] = *b; /* move: ownership transfers to `out` */
         continue;
      }

      /* Any prose preceding the first call is real assistant text; keep it, drop the
       * block when the model emitted nothing but the call. */
      if (p->parsed.content && p->parsed.content[0])
      {
         out[n_out].type = AIMEE_BLK_TEXT;
         out[n_out].text = strdup(p->parsed.content);
         out[n_out].cache_control = b->cache_control ? strdup(b->cache_control) : NULL;
         n_out++;
      }

      for (int c = 0; c < p->parsed.call_count; c++)
      {
         out[n_out].type = AIMEE_BLK_TOOL_USE;
         out[n_out].tool_id = strdup(p->parsed.calls[c].id);
         out[n_out].tool_name = strdup(p->parsed.calls[c].name);
         out[n_out].tool_input = args_to_json(p->parsed.calls[c].arguments);
         n_out++;
      }

      aimee_block_free_contents(b); /* the source TEXT block is fully consumed */
   }

   free(r->content);
   r->content = out;
   r->n_content = n_out;
   free_prepared(prepared, source_n_content);

   /* A rescued call IS a tool call: the stop reason must say so, or the caller
    * treats a tool-calling turn as a finished answer and never dispatches it. */
   r->stop_reason = AIMEE_STOP_TOOL_USE;

   aimee_ir_metric_inc(AIMEE_IR_M_RESCUE_RECOVERIES, AIMEE_WIRE_UNKNOWN);
   return rescued;
}

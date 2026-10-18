#define _POSIX_C_SOURCE 200809L
/* Canonical owned rescue assembly. Native dialect policy stays in Go. */
#include <aimee/delegates/aimee_ir_rescue.h>
#include <aimee/delegates/delegate_xml_fallback.h>
#include <aimee/ir/aimee_ir_metrics.h>
#include <stdlib.h>
#include <string.h>

int aimee_ir_rescue_tool_calls(aimee_response_t *r, int allow_json)
{
   if (!r || !r->content || r->n_content <= 0 || aimee_ir_response_has_tool_use(r))
      return 0;
   if (r->n_content > AIMEE_IR_MAX_BLOCKS)
      return -1;
   const int source_count = r->n_content;
   aimee_message_t *prepared = calloc((size_t)r->n_content, sizeof *prepared);
   if (!prepared)
      return -1;
   int rescued = 0, total = r->n_content, result = -1;
   for (int i = 0; i < r->n_content; i++)
   {
      aimee_block_t *source = &r->content[i];
      if (source->type != AIMEE_BLK_TEXT || !source->text)
         continue;
      int n = delegate_rescue_message(source->text, &prepared[i], allow_json);
      if (n < 0)
         goto done;
      if (!n)
         continue;
      rescued += n;
      if (prepared[i].n_blocks > AIMEE_IR_MAX_BLOCKS - total + 1)
         goto done;
      total += prepared[i].n_blocks - 1;
      for (int j = 0; j < prepared[i].n_blocks; j++)
         prepared[i].blocks[j].context = source->context;
      aimee_block_t *last = &prepared[i].blocks[prepared[i].n_blocks - 1];
      free(last->cache_control);
      free(last->cache_ttl);
      last->cache_control = source->cache_control ? strdup(source->cache_control) : NULL;
      last->cache_ttl = source->cache_ttl ? strdup(source->cache_ttl) : NULL;
      if ((source->cache_control && !last->cache_control) ||
          (source->cache_ttl && !last->cache_ttl))
         goto done;
   }
   if (!rescued)
   {
      result = 0;
      goto done;
   }
   aimee_block_t *blocks = calloc((size_t)total, sizeof *blocks);
   if (!blocks)
      goto done;
   int at = 0;
   for (int i = 0; i < r->n_content; i++)
   {
      if (!prepared[i].n_blocks)
      {
         blocks[at++] = r->content[i];
         continue;
      }
      memcpy(blocks + at, prepared[i].blocks, (size_t)prepared[i].n_blocks * sizeof *blocks);
      at += prepared[i].n_blocks;
      free(prepared[i].blocks);
      prepared[i].blocks = NULL;
      prepared[i].n_blocks = 0;
      aimee_block_free_contents(&r->content[i]);
   }
   free(r->content);
   r->content = blocks;
   r->n_content = total;
   r->stop_reason = AIMEE_STOP_TOOL_USE;
   aimee_ir_metric_inc(AIMEE_IR_M_RESCUE_RECOVERIES, AIMEE_WIRE_UNKNOWN);
   result = rescued;
done:
   for (int i = 0; i < source_count; i++)
      aimee_message_free(&prepared[i]);
   free(prepared);
   return result;
}

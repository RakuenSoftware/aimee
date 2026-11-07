#include <aimee/delegates/delegate_xml_fallback.h>
#include <string.h>
#include "cJSON.h"
static delegate_rescue_provider_fn g_rescue_provider;
void delegate_register_rescue_provider(delegate_rescue_provider_fn provider)
{
   g_rescue_provider = provider;
}
int delegate_rescue_message(const char *text, aimee_message_t *out, int allow_json)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!text || !g_rescue_provider)
      return 0;
   int found = g_rescue_provider(text, allow_json, 0, out), count = 0;
   if (found <= 0)
   {
      aimee_message_free(out);
      return found;
   }
   if (!out->role || strcmp(out->role, "assistant") || out->n_blocks < 1 ||
       out->n_blocks > AIMEE_IR_MAX_BLOCKS || !out->blocks)
      goto fail;
   for (int i = 0; i < out->n_blocks; i++)
   {
      const aimee_block_t *b = &out->blocks[i];
      if (!aimee_ir_block_valid(b) ||
          (b->type != AIMEE_BLK_TEXT && b->type != AIMEE_BLK_TOOL_USE) ||
          (b->type == AIMEE_BLK_TOOL_USE && !cJSON_IsObject(b->tool_input)))
         goto fail;
      count += b->type == AIMEE_BLK_TOOL_USE;
   }
   if (count != found)
      goto fail;
   return found;
fail:
   aimee_message_free(out);
   return -1;
}

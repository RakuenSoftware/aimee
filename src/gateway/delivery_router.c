#include "gateway_platform.h"
#include "delivery_router.h"
#include "gateway_ir.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
int delivery_router_send_ir(gateway_ctx_t *ctx, delivery_target_t *target,
                            const aimee_message_t *messages, int count)
{
   (void)ctx;
   if (!target || !target->platform[0] || delivery_target_is_origin(target) || !messages ||
       count < 1)
      return -1;
   for (int m = 0; m < count; m++)
      if (gateway_message_admit(messages + m))
         return -1;
   platform_adapter_t *adapter = gateway_platform_find(target->platform);
   if (!adapter)
      return -1;
   /* This is the platform API edge: render text once, preserving Markdown and
    * byte ordering across authored messages. Metadata remains in the mirror. */
   char *text = gateway_message_text_view(messages, count);
   if (!text)
      return -1;
   int rc = text[0] && (!adapter->send_text || adapter->send_text(adapter, target, text));
   free(text);
   if (rc)
      return -1;
   for (int m = 0; m < count; m++)
      for (int i = 0; i < messages[m].n_blocks; i++)
      {
         const aimee_block_t *b = messages[m].blocks + i;
         if (!aimee_ir_block_model_visible(b) ||
             (b->type != AIMEE_BLK_IMAGE && b->type != AIMEE_BLK_DOCUMENT))
            continue;
         /* Existing attachment transport supports owned local files only. No
          * URL fetching or invented platform media capability. */
         const char *path = gateway_asset_path(b);
         if (!path || !adapter->send_attachment)
         {
            LOG_WARN("gateway", "platform attachment projection unavailable");
            return -1;
         }
         if (adapter->send_attachment(adapter, target, path,
                                      b->media_type ? b->media_type : "application/octet-stream"))
            return -1;
      }
   return 0;
}

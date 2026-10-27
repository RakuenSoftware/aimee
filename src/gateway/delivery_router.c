/* delivery_router.c: gateway outbound delivery dispatch.
 * Routes outbound messages to the appropriate platform adapter using the
 * global registry (gateway_platform_find).  Adapter lifecycle and
 * registration is handled by platform_registry.c.
 */
#include "gateway_platform.h"
#include "delivery_router.h"

#include <string.h>

int delivery_router_send(gateway_ctx_t *ctx, delivery_target_t *target, const char *text,
                         const attachment_t *attachments, int attachment_count)
{
   (void)ctx;

   if (!target || !target->platform[0] || delivery_target_is_origin(target))
      return -1;
   if (attachment_count < 0)
      return -1;

   int has_text = text && text[0];
   int has_attachments = attachments && attachment_count > 0;
   if (!has_text && !has_attachments)
      return -1;

   platform_adapter_t *adapter = gateway_platform_find(target->platform);
   if (!adapter)
      return -1;

   if (has_text)
   {
      if (!adapter->send_text || adapter->send_text(adapter, target, text) != 0)
         return -1;
   }

   for (int i = 0; i < attachment_count; i++)
   {
      if (!attachments[i].path[0])
         return -1;
      if (!adapter->send_attachment ||
          adapter->send_attachment(adapter, target, attachments[i].path, attachments[i].mime) != 0)
         return -1;
   }

   return 0;
}

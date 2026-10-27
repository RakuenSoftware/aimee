/* delivery_router.h: gateway outbound delivery dispatcher. */
#ifndef AIMEE_GATEWAY_DELIVERY_ROUTER_H
#define AIMEE_GATEWAY_DELIVERY_ROUTER_H 1

#include "gateway_ctx.h"

/* Dispatch text and/or attachments to the appropriate platform adapter
 * based on the delivery target's platform field. Returns 0 on success, -1
 * on error (unknown platform, null inputs, etc.). */
int delivery_router_send(gateway_ctx_t *ctx, delivery_target_t *target, const char *text,
                         const attachment_t *attachments, int attachment_count);

#endif /* AIMEE_GATEWAY_DELIVERY_ROUTER_H */

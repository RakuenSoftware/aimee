/* Actual external platform rendering boundary. Source IR is never mutated. */
#ifndef AIMEE_GATEWAY_DELIVERY_ROUTER_H
#define AIMEE_GATEWAY_DELIVERY_ROUTER_H 1
#include "gateway_ctx.h"
#include <aimee/ir/aimee_ir.h>
int delivery_router_send_ir(gateway_ctx_t *ctx, delivery_target_t *target,
                            const aimee_message_t *messages, int count);
#endif

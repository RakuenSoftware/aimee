/* gateway_ctx.h: gateway runtime context. */
#ifndef AIMEE_GATEWAY_CTX_H
#define AIMEE_GATEWAY_CTX_H 1

#include "gateway_platform.h"
#include "delivery_target.h"
#include <stddef.h>
#include <aimee/ir/aimee_ir.h>

typedef struct gateway_ctx gateway_ctx_t;

/* Initialise / tear down the gateway context. */
gateway_ctx_t *gateway_ctx_new(void);
void gateway_ctx_free(gateway_ctx_t *ctx);

/* Handle an inbound message from a platform adapter.
 * Parses delivery target, sends to server, routes response back. */
int gateway_handle_message_ir(gateway_ctx_t *ctx, const session_source_t *src,
                              const aimee_message_t *message);
int gateway_send_text(gateway_ctx_t *ctx, const delivery_target_t *target, const char *text);

/* Authorise a user for a given source. Returns 0 on allow, -1 on deny. */
int gateway_authorize_user(gateway_ctx_t *ctx, const session_source_t *src);

/* Build session key from source. Caller must free result. */
char *gateway_session_key(const session_source_t *src);

/* Parse the argument tail of the `/pr` slash command — the text AFTER `/pr `,
 * i.e. "<workspace> [title...]". Writes the (first whitespace-delimited)
 * workspace token into ws[ws_cap] and the trimmed remainder as the PR title into
 * title[title_cap] (empty when no title was given). Returns 0 on success, -1 if
 * no workspace token is present. Pure — unit-tested. */
int gateway_parse_pr_command(const char *args, char *ws, size_t ws_cap, char *title,
                             size_t title_cap);

/* Set typing indicator on a delivery target. */
int gateway_set_typing(gateway_ctx_t *ctx, const delivery_target_t *target, int typing);

#endif /* AIMEE_GATEWAY_CTX_H */

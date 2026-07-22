/* kb_reqctx.h: per-request (thread-local) authenticated actor principal.
 *
 * The request router (kb_http_route_ex) resolves the caller's actor principal
 * after verification and stashes it here so tenant-aware handlers (e.g. the
 * /v1/team routes) can read it without threading it through every signature.
 * Set at request start, cleared at request end. Slice-4 minimal form (actor only);
 * the transport principal + full composite context wire in with P2's egress path. */
#ifndef DEC_KB_REQCTX_H
#define DEC_KB_REQCTX_H 1

#include "kb_identity.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Set/clear the current thread's actor principal (copied in). */
   void kb_reqctx_set_actor(const kb_principal_t *actor);
   void kb_reqctx_clear(void);

   /* The current actor, or NULL if none set / unauthenticated. */
   const kb_principal_t *kb_reqctx_actor(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_REQCTX_H */

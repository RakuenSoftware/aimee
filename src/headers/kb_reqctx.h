/* kb_reqctx.h: per-request (thread-local) authenticated actor principal.
 *
 * The request router (kb_http_route_ex) resolves the caller's actor principal
 * after verification and stashes it here so tenant-aware handlers (e.g. the
 * /v1/team routes) can read it without threading it through every signature.
 * The actor may come from a KB-verified caller credential or from canonical caller
 * context carried over a fully authenticated service transport. Set at request
 * start and cleared at request end. */
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

   /* Verified credential scope for the current request.  Unlike actor identity,
    * scoped service credentials deliberately have no actor principal, but code
    * reads still need their authenticated project scope to resolve an omitted
    * project without falling back to every indexed repository. */
   void kb_reqctx_set_verified_scope(const char *kind, const char *id);
   int kb_reqctx_verified_scope(const char **kind, const char **id);

   /* The request's Content-Type header, for the handful of routes whose SECURITY
    * depends on it rather than merely their parsing. Set by the connection layer
    * before routing and cleared after; "" when the request carried none.
    *
    * The PAM login route is the reason this exists: a browser can send exactly
    * text/plain, application/x-www-form-urlencoded and multipart/form-data
    * cross-origin without a preflight, so a route that accepts a JSON body under
    * any of those is reachable from an attacker's form. Deciding that needs the
    * header, and threading a new parameter through kb_http_route_ex would touch
    * every route and every test call site to serve two of them.
    *
    * Independent of kb_reqctx_clear(), which clears the authenticated actor: the
    * content type is known BEFORE authentication and is needed by the pre-auth
    * bootstrap routes, which run before the actor is resolved. */
   void kb_reqctx_set_content_type(const char *value);
   void kb_reqctx_clear_content_type(void);
   const char *kb_reqctx_content_type(void);

   /* 1 iff the current request's Content-Type is JSON ("application/json",
    * case-insensitive, parameters such as "; charset=utf-8" allowed). A missing
    * or empty header is NOT json: a route that requires JSON must refuse a
    * request that never said it was sending any. */
   int kb_reqctx_content_type_is_json(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_REQCTX_H */

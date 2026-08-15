/* kb_scope.h: bearer-token scope parsing and per-request scope authorization
 * for the aimee-kb /v1 API.
 *
 * A configured bearer token may be self-describing:
 *   scope:<kind>:<id>:<secret>   — a scoped token (e.g. scope:project:foo:s3cr3t)
 *   <secret>                     — an unscoped/admin token (full access)
 *
 * One kind is broader than an exact match: `service` is the data-plane identity
 * a companion service (aimee-server) carries. It reaches ANY project or
 * workspace — it indexes and searches on behalf of the whole deployment — but
 * it is still SCOPED, so every administrative gate that refuses a scoped
 * credential refuses it too (notably the /v1/maintenance routes). It is not
 * a second spelling of admin: the point of giving aimee-server one is that
 * holding it does not confer the ability to purge or repair the store.
 *
 * Auth (does the presented secret match) is separate from authorization
 * (may a token scoped <kind>:<id> touch a resource scoped <req_kind>:<req_id>).
 * The decision logic here is pure so it is fully unit-testable without a live
 * server or DB.
 *
 * See docs/proposals/accepted/aimee-kb-service-and-public-api.md
 * (AC: "Bearer token with scope project:X cannot read or write artifacts at
 * workspace:Y"). */
#ifndef DEC_KB_SCOPE_H
#define DEC_KB_SCOPE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse a configured bearer token into its scope (kind/id) and secret.
    * For "scope:<kind>:<id>:<secret>" the three parts are split out. For any
    * other string the whole token is the secret and kind/id are empty (admin).
    * Output buffers are always NUL-terminated. Returns 0 (always succeeds;
    * malformed scoped tokens degrade to admin so a typo never locks everyone
    * out — the secret still has to match). */
   int kb_scope_token_parse(const char *token, char *scope_kind, size_t kind_len, char *scope_id,
                            size_t id_len, char *secret, size_t secret_len);

/* The scope kind carried by a companion service's data-plane bearer. */
#define KB_SCOPE_KIND_SERVICE "service"

/* The scope an aimee-server's OUTBOUND identity to aimee-kb carries — the one
 * credential it uses to reach the data plane. Defined once because it is minted
 * by two different provisioning paths (the managed installer signs a CSR against
 * the kb's CA; a distributed server redeems an aimee:// enrolment token) and
 * matched by the kb_enrollments / kb_server_registry SQL. Two literals drifting
 * apart would silently unregister a live server.
 *
 * It is a `service` scope. At the mTLS seam the certificate CN must match the
 * independently verified bearer's derived scope, and that bearer reaches the
 * router as a data-plane caller: any project or workspace, refused by every
 * administrative gate. It used to be the bare word "p5-server-client", which
 * has no ':' and therefore made the certificate-derived synthetic credential
 * UNSCOPED — the install owner, past every gate. Synthetic credentials are no
 * longer accepted; two proxied owner-only routes that once needed that breadth
 * (write-tier grants and repo trust) are also gone.
 *
 * kb_enrollments rows and the SQL that matches them carry this string, so
 * changing it is schema-visible: a certificate issued under the old CN no longer
 * matches and its server must re-enrol. */
#define KB_SERVER_CLIENT_SCOPE "service:aimee-server"

   /* Authorization decision. A token scoped (token_kind:token_id) may access a
    * resource scoped (req_kind:req_id) iff:
    *   - the token is unscoped (token_kind empty) — admin, full access; or
    *   - the token is a `service` token and the resource is a project or a
    *     workspace (any id — a service indexes the whole deployment); or
    *   - token_kind == req_kind AND token_id == req_id (exact match).
    * Returns 1 if authorized, 0 if denied. */
   int kb_scope_authorized(const char *token_kind, const char *token_id, const char *req_kind,
                           const char *req_id);

   /* Extract the scope a request targets, from the query string
    * (scope=<kind>:<id>, project=<id>, or workspace=<id>) or the JSON body
    * ("scope_kind"+"scope_id", "scope_user" → kind=user, or "project" →
    * kind=project). query_string and body may be NULL. Writes the resolved
    * kind/id into the buffers. Returns 1 if a target scope was found, 0 if the
    * request names no scope.
    *
    * Body and query are both consulted because a POST names its target project
    * in the body — a caller must not be able to escape scope enforcement just
    * by omitting a query string. */
   int kb_scope_request_target(const char *query_string, const char *body, char *kind,
                               size_t kind_len, char *id, size_t id_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_SCOPE_H */

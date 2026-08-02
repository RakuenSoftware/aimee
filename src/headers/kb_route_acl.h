/* kb_route_acl.h — static per-scope route allowlist for aimee-kb.
 *
 * The web console holds a single `scope:console-admin:<id>:<secret>` bearer and
 * is authorized ONLY for a fixed, compile-time set of (method, route) pairs —
 * the containment model from the kb-web-console proposal. Enforced server-side
 * in the kb HTTP dispatch (defence-in-depth with the Go console's own role
 * gate), so a compromised console cannot reach arbitrary kb routes.
 *
 * Matching is segment-exact: a "{id}" pattern segment matches exactly one
 * non-empty, slash-free path segment; every other segment must match literally,
 * and the segment counts must be equal. Encoded paths, extra/trailing segments,
 * sibling paths, and wrong methods therefore cannot widen access. Pure string
 * logic — no DB, no network, no allocation.
 */
#ifndef KB_ROUTE_ACL_H
#define KB_ROUTE_ACL_H

/* The scope kind carried by the console's bearer (scope:console-admin:...). */
#define KB_SCOPE_KIND_CONSOLE_ADMIN "console-admin"

/* Returns 1 if a console-admin credential may call (method, path); 0 otherwise.
 * `method` is the upper-case HTTP method; `path` is the request path with any
 * query string already stripped (a single trailing slash is tolerated). */
int kb_route_acl_console_admin_allows(const char *method, const char *path);

/* 1 iff `path` is in the /v1/maintenance family, which the dispatch gates on the
 * owner credential as a whole.
 *
 * Every route there is destructive: purge fans out deletes across nine stores,
 * clear drops a project's chunks, repair rewrites the index, reconcile prunes
 * orphans. Matching by PREFIX rather than enumerating routes is deliberate — a
 * maintenance route added later is refused until someone opens it, instead of
 * shipping open. A scoped service bearer (an aimee-server's, say) must not be
 * able to purge a project out of the knowledge store.
 *
 * The caller pairs this with the same rule the /v1/code/project lifecycle routes
 * use: a real authenticated actor AND an unscoped credential. That refuses in
 * auth-off mode too (no actor is manufactured there) — a destructive maintenance
 * op fails closed rather than inheriting "everything open". */
int kb_route_acl_is_maintenance(const char *path);

#endif /* KB_ROUTE_ACL_H */

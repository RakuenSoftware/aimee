/* kb_route_acl.h — KB authorization seams for scoped control-web requests and
 * owner-only maintenance routes. */
#ifndef KB_ROUTE_ACL_H
#define KB_ROUTE_ACL_H

/* The scope kind carried by the console's bearer (scope:console-admin:...). */
#define KB_SCOPE_KIND_CONSOLE_ADMIN "console-admin"

/* Implemented by aimee-kb's event-bus adapter. The route layer never evaluates
 * the control-web allowlist locally. */
typedef int (*kb_route_authorization_provider_fn)(const char *method, const char *path,
                                                  int *allowed);

void kb_route_acl_register_authorization_provider(kb_route_authorization_provider_fn provider);

/* Ask the separately supervised control-web module whether a console-admin
 * credential may call (method, path). Returns 0 with *allowed set to 0 or 1, or
 * -1 when no provider is registered or the event-bus decision fails. There is
 * no local authorization fallback. */
int kb_route_acl_console_admin_authorize(const char *method, const char *path, int *allowed);

/* 1 iff `path` is in the /v1/maintenance family, which the dispatch gates on the
 * owner credential as a whole. This is core destructive-operation policy, not
 * a control-web route decision. */
int kb_route_acl_is_maintenance(const char *path);

#endif /* KB_ROUTE_ACL_H */

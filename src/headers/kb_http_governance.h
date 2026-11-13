/* kb_http_governance.h — kb HTTP routes for the console governance surface:
 * decision records (list/detail/author) and the policy-verdict action audit.
 * Reached only with a console-admin credential the route ACL has authorized. */
#ifndef KB_HTTP_GOVERNANCE_H
#define KB_HTTP_GOVERNANCE_H

/* Handle a governance route (/v1/decisions*, /v1/audit/actions). Returns the HTTP
 * status if (method, path) is a governance route, or -1 if it is not. */
int kb_http_governance_route(const char *method, const char *path, const char *query_string,
                             const char *body, char *out_buf, int out_cap);

#endif /* KB_HTTP_GOVERNANCE_H */

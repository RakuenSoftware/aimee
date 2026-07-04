/* kb_http_accounts.h — kb HTTP routes for the console accounts surface:
 * enrollment listing, certificate revocation, and scope enumeration. Reached
 * only with a console-admin credential the route ACL has already authorized. */
#ifndef KB_HTTP_ACCOUNTS_H
#define KB_HTTP_ACCOUNTS_H

/* Handle an accounts route (/v1/enrollments*, /v1/scopes). Returns the HTTP
 * status if (method, path) is an accounts route, or -1 if it is not. */
int kb_http_accounts_route(const char *method, const char *path, const char *query_string,
                           char *out_buf, int out_cap);

#endif /* KB_HTTP_ACCOUNTS_H */

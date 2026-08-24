/* server_http_routes_workspace.h: the /v1/workspaces route handlers.
 * See server_http_routes_workspace.c. */
#ifndef AIMEE_SERVER_HTTP_ROUTES_WORKSPACE_H
#define AIMEE_SERVER_HTTP_ROUTES_WORKSPACE_H 1

#include "server_http_internal.h" /* route_req_t */

int rh_workspaces_register(const route_req_t *rq, char *resp, int cap);
int rh_workspace_get(const route_req_t *rq, char *resp, int cap);
int rh_workspace_remove(const route_req_t *rq, char *resp, int cap);

#endif /* AIMEE_SERVER_HTTP_ROUTES_WORKSPACE_H */

#ifndef AIMEE_WFE_HTTP_PROXY_H
#define AIMEE_WFE_HTTP_PROXY_H

/* Forward an already-authorized public /v1 workflow request to the private Go
 * control-plane socket. Identity and the CAP_WORKFLOW_ADMIN decision are
 * attested separately. Returns the upstream HTTP status and copies its JSON
 * response into resp. */
int wfe_http_proxy_request(const char *method, const char *path, const char *query,
                           const char *body, int body_len, const char *principal,
                           int workflow_operator, char *resp, int resp_cap);

#endif

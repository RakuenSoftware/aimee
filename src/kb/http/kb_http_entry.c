#include "kb_http.h"
#include "kb_identity.h"

int kb_http_route_ex_context_impl(const char *method, const char *path, const char *query_string,
                                  const char *auth_header, const char *bearer_token,
                                  const char *body, int body_len,
                                  const kb_principal_t *asserted_actor,
                                  const kb_request_context_t *resolved, char *out_buf, int out_cap);

int kb_http_route_ex(const char *method, const char *path, const char *query_string,
                     const char *auth_header, const char *bearer_token, const char *body,
                     int body_len, char *out_buf, int out_cap)
{
   return kb_http_route_ex_context_impl(method, path, query_string, auth_header, bearer_token, body,
                                        body_len, NULL, NULL, out_buf, out_cap);
}

int kb_http_route_ex_with_actor(const char *method, const char *path, const char *query_string,
                                const char *auth_header, const char *bearer_token, const char *body,
                                int body_len, const kb_principal_t *asserted_actor, char *out_buf,
                                int out_cap)
{
   return kb_http_route_ex_context_impl(method, path, query_string, auth_header, bearer_token, body,
                                        body_len, asserted_actor, NULL, out_buf, out_cap);
}

int kb_http_route_ex_with_context(const char *method, const char *path, const char *query_string,
                                  const char *auth_header, const char *bearer_token,
                                  const char *body, int body_len,
                                  const kb_request_context_t *resolved, char *out_buf, int out_cap)
{
   const kb_principal_t *actor = resolved && resolved->has_actor ? &resolved->actor : NULL;
   return kb_http_route_ex_context_impl(method, path, query_string, auth_header, bearer_token, body,
                                        body_len, actor, resolved, out_buf, out_cap);
}

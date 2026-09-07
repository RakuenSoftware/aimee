/* Provider/model API compatibility transport. All management policy is Go-owned. */
#include "server.h"
#include "providers_client.h"
#include "model_registry.h"
#include "cJSON.h"
#include <aimee/delegates/delegate_credentials.h>
#include <string.h>
static int proxy(server_conn_t *conn, cJSON *req, const char *op)
{
   cJSON *reply = providers_module_request(op, req, conn->vault_principal, 0);
   if (!reply)
      return server_send_error_kind(conn, SERVER_ERR_UNAVAILABLE, "providers module unavailable",
                                    NULL);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "status"));
   if (status && strcmp(status, "error") == 0)
   {
      const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "message"));
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "kind"));
      int rc = server_send_error_kind(conn, kind ? kind : SERVER_ERR_UNAVAILABLE,
                                      message ? message : "provider operation failed", NULL);
      cJSON_Delete(reply);
      return rc;
   }
   return server_send_ok(conn, reply);
}
int handle_provider_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.list");
}
int handle_provider_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.show");
}
int handle_provider_models(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.models");
}
int handle_provider_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.test");
}
int handle_provider_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.get");
}
int handle_provider_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "provider.set");
}
int handle_provider_quota(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   const char *name = cJSON_IsString(jname) && jname->valuestring[0] ? jname->valuestring : NULL;
   char quota[4096];
   int n = delegate_credentials_format_quota(name, quota, sizeof(quota));
   if (n < 0)
      return server_send_error(conn, "failed to format credential quota state", NULL);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   if (name)
      cJSON_AddStringToObject(resp, "provider", name);
   cJSON_AddNumberToObject(resp, "credential_count", n);
   cJSON_AddStringToObject(resp, "quota", quota);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_model_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "metadata.list");
}
int handle_model_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "metadata.show");
}
int handle_model_refresh(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return proxy(conn, req, "metadata.refresh");
}

/* test_server_error_kind.c: server error envelopes obtain their HTTP mapping
 * only from the runtime-web process contract. */
#include "cJSON.h"
#include "server.h"
#include "server/server_error_kind.h"

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/runtime-web/module_api.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

extern aimee_module_status_t aimee_runtime_web_module_handler(const aimee_module_invocation_t *,
                                                              const uint8_t *, uint32_t, uint8_t *,
                                                              uint32_t, uint32_t *, void *);

static cJSON *g_response;

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   cJSON_Delete(g_response);
   g_response = cJSON_Duplicate(response, 1);
   return 0;
}

static int runtime_web_provider(const char *kind, uint32_t *http_status)
{
   uint8_t request[AIMEE_RUNTIME_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_RUNTIME_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_RUNTIME_WEB_STAGE_CLASSIFY};
   if (aimee_runtime_web_request_encode(kind, request, sizeof(request)) != 0 ||
       aimee_runtime_web_module_handler(&invocation, request, sizeof(request), response,
                                        sizeof(response), &response_len,
                                        NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_runtime_web_response_decode(response, response_len, http_status);
}

static int failing_provider(const char *kind, uint32_t *http_status)
{
   (void)kind;
   (void)http_status;
   return -1;
}

static int invalid_provider(const char *kind, uint32_t *http_status)
{
   (void)kind;
   *http_status = 200;
   return 0;
}

static void expect_status(const char *kind, int expected)
{
   server_conn_t conn = {0};
   assert(server_send_error_kind(&conn, kind, "message", "request-1") == 0);
   assert(g_response != NULL);
   assert(cJSON_GetObjectItem(g_response, "http_status")->valueint == expected);
   assert(cJSON_IsString(cJSON_GetObjectItem(g_response, "message")));
   assert(cJSON_IsString(cJSON_GetObjectItem(g_response, "request_id")));
}

int main(void)
{
   server_error_kind_register_http_status_provider(runtime_web_provider);
   expect_status(SERVER_ERR_INVALID_ARGUMENT, 400);
   expect_status(SERVER_ERR_NOT_FOUND, 404);
   expect_status(SERVER_ERR_PERMISSION_DENIED, 403);
   expect_status(SERVER_ERR_UNAVAILABLE, 503);
   expect_status("unknown", 502);
   expect_status(NULL, 502);

   server_conn_t conn = {0};
   server_error_kind_register_http_status_provider(NULL);
   assert(server_send_error_kind(&conn, SERVER_ERR_INVALID_ARGUMENT, "message", NULL) == 0);
   assert(cJSON_GetObjectItem(g_response, "http_status") == NULL);

   server_error_kind_register_http_status_provider(failing_provider);
   assert(server_send_error_kind(&conn, SERVER_ERR_INVALID_ARGUMENT, "message", NULL) == 0);
   assert(cJSON_GetObjectItem(g_response, "http_status") == NULL);

   server_error_kind_register_http_status_provider(invalid_provider);
   assert(server_send_error_kind(&conn, SERVER_ERR_INVALID_ARGUMENT, "message", NULL) == 0);
   assert(cJSON_GetObjectItem(g_response, "http_status") == NULL);

   cJSON_Delete(g_response);
   printf("server_error_kind: all tests passed\n");
   return 0;
}

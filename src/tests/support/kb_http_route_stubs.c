/* Link-only stubs for routes outside test_kb_http_routes.c's focused surface. */
#include "kb_http_budget.h"
#include "kb_http_insights.h"
#include "kb_http_models.h"
#include "kb_http_rate.h"
#include "kb/http/kb_http_servers.h"
#include "kb_http_telemetry.h"
#include "db2/server_registry.h"
#include <stdio.h>

int kb_http_telemetry_token_route(const char *method, const char *path, const char *query_string,
                                  const char *body, const char *presented, char *out_buf,
                                  int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)presented;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_models_route(const char *method, const char *path, const char *body, char *out_buf,
                         int out_cap)
{
   (void)method;
   (void)path;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_insights_route(const char *method, const char *path, const char *query_string,
                           char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_budget_route(const char *method, const char *path, const char *query_string,
                         const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_rate_route(const char *method, const char *path, const char *query_string,
                       const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_telemetry_route(const char *method, const char *path, const char *query_string,
                            const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_servers_route(const char *method, const char *path, const char *query_string,
                          char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int g_test_registry_heartbeat_allow;
char g_test_registry_server_id[128], g_test_registry_issuer[601], g_test_registry_serial[129],
    g_test_registry_fingerprint[65];

int db2_server_registry_heartbeat(const char *server_id, const char *issuer, const char *serial,
                                  const char *fingerprint, const char *health, const char *version)
{
   snprintf(g_test_registry_server_id, sizeof(g_test_registry_server_id), "%s", server_id);
   snprintf(g_test_registry_issuer, sizeof(g_test_registry_issuer), "%s", issuer);
   snprintf(g_test_registry_serial, sizeof(g_test_registry_serial), "%s", serial);
   snprintf(g_test_registry_fingerprint, sizeof(g_test_registry_fingerprint), "%s", fingerprint);
   (void)health;
   (void)version;
   return g_test_registry_heartbeat_allow ? 0 : -1;
}

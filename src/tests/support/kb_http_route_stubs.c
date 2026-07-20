/* Link-only stubs for routes outside test_kb_http_routes.c's focused surface. */
#include "kb_http_budget.h"
#include "kb_http_insights.h"
#include "kb_http_models.h"
#include "kb_http_rate.h"
#include "kb_http_telemetry.h"

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

#include "kb/http/kb_http_common.h"
#include "cJSON.h"
#include "kb_reqctx.h"
#include "modules/kb/c/kb_store_tenant.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static kb_principal_t actor;
static int actor_present, begin_result, begin_calls;
const kb_principal_t *kb_reqctx_actor(void)
{
   return actor_present ? &actor : NULL;
}
int kb_store_tenant_scope_begin(const kb_principal_t *principal, int64_t team)
{
   assert(principal == &actor && team == 0);
   ++begin_calls;
   return begin_result;
}

static void scope_failures(void)
{
   char out[128];
   int http = 0;
   assert(kb_http_begin_actor_scope(out, sizeof(out), &http) == -1);
   assert(http == 401 && !begin_calls && strstr(out, "authentication required"));
   actor_present = 1;
   static const int errors[] = {KB_STORE_ERR_TENANT_REQUIRES_PG,
                                KB_STORE_ERR_TENANT_UNAUTHENTICATED, KB_STORE_ERR_TENANT_DENIED,
                                KB_STORE_ERR_TENANT_NO_CONN, KB_STORE_ERR_TENANT_BEGIN};
   static const int statuses[] = {503, 401, 403, 500, 500};
   for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
   {
      begin_result = errors[i];
      assert(kb_http_begin_actor_scope(out, sizeof(out), &http) == -1);
      assert(http == statuses[i] && strstr(out, "tenant scope failed"));
   }
   begin_result = 0;
   http = 123;
   assert(kb_http_begin_actor_scope(out, sizeof(out), &http) == 0 && http == 123);
   assert(begin_calls == 6);
}

static void query_and_numbers(void)
{
   char out[8] = "x";
   assert(!kb_http_query_param(NULL, "id", out, sizeof(out)) && !*out);
   assert(!kb_http_query_param("id2=x&other=id", "id", out, sizeof(out)) && !*out);
   assert(kb_http_query_param("id=&id=second", "id", out, sizeof(out)) && !*out);
   assert(kb_http_query_param("other=x&id=a%20b+c", "id", out, sizeof(out)) &&
          !strcmp(out, "a%20b+c"));
   assert(kb_http_query_param("id=12345678", "id", out, sizeof(out)) && !strcmp(out, "1234567"));
   out[0] = 'x';
   assert(!kb_http_query_param("id=y", "id", out, 0) && out[0] == 'x');
   assert(kb_http_query_param("id=y", "id", out, 1) && !*out);
   int64_t n = -1;
   assert(kb_http_parse_positive_int64("9223372036854775807", &n) && n == INT64_MAX);
   static const char *bad[] = {NULL, "", "0", "-1", "+1", " 1", "1x", "9223372036854775808"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      n = -1;
      assert(!kb_http_parse_positive_int64(bad[i], &n) && n == -1);
   }
}

int main(void)
{
   scope_failures();
   query_and_numbers();
   char out[128];
   assert(kb_http_error(out, sizeof(out), 405, "method not allowed") == 405 &&
          !strcmp(out, "{\"error\":\"method not allowed\"}"));
   assert(kb_http_emit(cJSON_CreateObject(), out, sizeof(out), 201) == 201 && !strcmp(out, "{}"));
   cJSON *large = cJSON_CreateObject();
   cJSON_AddStringToObject(large, "data", "longer than output");
   assert(kb_http_emit(large, out, 4, 200) == 500 && out[3] == '\0');
   puts("KB HTTP common: tenant failures, query boundaries and response framing passed");
   return 0;
}

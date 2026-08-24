/* test_osv_check.c: MCP package-manager OSV target inference. */
#include "osv_check.h"
#include "db1_client/mcp_osv_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_http_response;
static int g_http_status = -1;
static int g_http_calls;
static int g_cache_hit;
static db1_mcp_osv_cache_row_t g_cache_row;
static int g_cache_upserts;

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version,
                          int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)ttl_hours;
   if (!g_cache_hit)
      return 0;
   *out = g_cache_row;
   return 1;
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version,
                             const char *verdict, const char *advisory_ids)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   g_cache_upserts++;
   snprintf(g_cache_row.verdict, sizeof(g_cache_row.verdict), "%s", verdict);
   snprintf(g_cache_row.advisory_ids, sizeof(g_cache_row.advisory_ids), "%s",
            advisory_ids ? advisory_ids : "");
   return 0;
}

int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name,
                      const char *version, const char *verdict, const char *action,
                      const char *advisory_ids)
{
   (void)client_name;
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)verdict;
   (void)action;
   (void)advisory_ids;
   return 0;
}

int http_retry_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers, int max_attempts, int base_ms,
                    int max_ms)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   (void)max_attempts;
   (void)base_ms;
   (void)max_ms;
   g_http_calls++;
   if (g_http_response)
   {
      size_t len = strlen(g_http_response);
      *response_buf = malloc(len + 1);
      assert(*response_buf != NULL);
      memcpy(*response_buf, g_http_response, len + 1);
   }
   else
      *response_buf = NULL;
   return g_http_status;
}

static void assert_target(const char *const argv[], int argc, const char *ecosystem,
                          const char *name, const char *version)
{
   osv_target_t target;
   assert(osv_infer_target_from_argv(argc, argv, &target) == 0);
   assert(strcmp(target.ecosystem, ecosystem) == 0);
   assert(strcmp(target.name, name) == 0);
   assert(strcmp(target.version, version) == 0);
}

static void test_npm_launchers(void)
{
   const char *npx[] = {"npx", "-y", "@modelcontextprotocol/server-filesystem@1.2.3"};
   assert_target(npx, 3, "npm", "@modelcontextprotocol/server-filesystem", "1.2.3");

   const char *pnpm[] = {"pnpm", "dlx", "--silent", "@scope/pkg"};
   assert_target(pnpm, 4, "npm", "@scope/pkg", "");

   const char *bunx[] = {"/usr/bin/bunx", "plain-pkg@0.4.0"};
   assert_target(bunx, 2, "npm", "plain-pkg", "0.4.0");
}

static void test_pypi_launchers(void)
{
   const char *uvx[] = {"uvx", "--refresh", "mcp-server"};
   assert_target(uvx, 3, "PyPI", "mcp-server", "");

   const char *uv[] = {"uv", "tool", "run", "--refresh", "server-pkg"};
   assert_target(uv, 5, "PyPI", "server-pkg", "");
}

static void test_unknown_launchers(void)
{
   const char *local[] = {"/opt/bin/server", "--stdio"};
   osv_target_t target;
   assert(osv_infer_target_from_argv(2, local, &target) == -1);
   assert(osv_infer_target_from_argv(0, NULL, &target) == -1);
}

static void test_malware_response_filter(void)
{
   char ids[64];
   assert(osv_response_has_malware("{\"vulns\":[{\"id\":\"CVE-2026-1\"}]}", ids, sizeof(ids)) == 0);
   assert(ids[0] == '\0');
   assert(osv_response_has_malware("{\"vulns\":[{\"id\":\"MAL-2026-1\"},"
                                   "{\"id\":\"MAL-2026-2\"}]}",
                                   ids, sizeof(ids)) == 1);
   assert(strstr(ids, "MAL-2026-1") != NULL);
   assert(strstr(ids, "MAL-2026-2") != NULL);
}

static void test_query_verdicts(void)
{
   osv_target_t target;
   snprintf(target.ecosystem, sizeof(target.ecosystem), "%s", "npm");
   snprintf(target.name, sizeof(target.name), "%s", "pkg");
   target.version[0] = '\0';

   g_http_calls = 0;
   g_http_status = 200;
   g_http_response = "{\"vulns\":[]}";
   osv_result_t result = osv_query_target("https://example.invalid", &target, 10);
   assert(result.verdict == OSV_VERDICT_CLEAN);
   assert(g_http_calls == 1);

   g_http_status = 200;
   g_http_response = "{\"vulns\":[{\"id\":\"MAL-2026-9\"}]}";
   result = osv_query_target("https://example.invalid", &target, 10);
   assert(result.verdict == OSV_VERDICT_MALWARE);
   assert(strcmp(result.advisory_ids, "MAL-2026-9") == 0);

   g_http_status = 503;
   g_http_response = "{\"error\":\"no\"}";
   result = osv_query_target("https://example.invalid", &target, 10);
   assert(result.verdict == OSV_VERDICT_UNKNOWN);
}

static void test_cached_verdicts(void)
{
   osv_target_t target;
   snprintf(target.ecosystem, sizeof(target.ecosystem), "%s", "npm");
   snprintf(target.name, sizeof(target.name), "%s", "pkg");
   target.version[0] = '\0';

   memset(&g_cache_row, 0, sizeof(g_cache_row));
   snprintf(g_cache_row.verdict, sizeof(g_cache_row.verdict), "%s", "malware");
   snprintf(g_cache_row.advisory_ids, sizeof(g_cache_row.advisory_ids), "%s", "MAL-2026-1");
   g_cache_hit = 1;
   g_http_calls = 0;
   osv_result_t result = osv_check_cached("https://example.invalid", &target, 24, 0, 10);
   assert(result.verdict == OSV_VERDICT_MALWARE);
   assert(strcmp(result.advisory_ids, "MAL-2026-1") == 0);
   assert(g_http_calls == 0);

   g_cache_hit = 0;
   g_http_calls = 0;
   g_cache_upserts = 0;
   g_http_status = 200;
   g_http_response = "{\"vulns\":[]}";
   result = osv_check_cached("https://example.invalid", &target, 24, 0, 10);
   assert(result.verdict == OSV_VERDICT_CLEAN);
   assert(g_http_calls == 1);
   assert(g_cache_upserts == 1);

   g_http_calls = 0;
   result = osv_check_cached("https://example.invalid", &target, 24, 1, 10);
   assert(result.verdict == OSV_VERDICT_UNKNOWN);
   assert(g_http_calls == 0);
}

int main(void)
{
   printf("osv_check: ");
   test_npm_launchers();
   test_pypi_launchers();
   test_unknown_launchers();
   test_malware_response_filter();
   test_query_verdicts();
   test_cached_verdicts();
   printf("all tests passed\n");
   return 0;
}

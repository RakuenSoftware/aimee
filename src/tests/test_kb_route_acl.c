/* test_kb_route_acl.c — unit tests for the console-admin route allowlist
 * (src/kb/http/kb_route_acl.c): segment-exact matching, {id} wildcards, and the
 * denial of wrong methods, sibling/extra segments, encoded paths, and query junk. */
#include "kb_route_acl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_allowed_routes(void)
{
   assert(kb_route_acl_console_admin_allows("GET", "/v1/console/overview"));
   assert(kb_route_acl_console_admin_allows("POST", "/v1/enroll"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/enrollments"));
   assert(kb_route_acl_console_admin_allows("POST", "/v1/enrollments/abc123/revoke"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/config/oidc"));
   assert(kb_route_acl_console_admin_allows("PUT", "/v1/config/oidc"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/scopes"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/decisions"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/decisions/42"));
   assert(kb_route_acl_console_admin_allows("POST", "/v1/decisions"));
   assert(kb_route_acl_console_admin_allows("POST", "/v1/decisions/42/supersede"));
   assert(kb_route_acl_console_admin_allows("GET", "/v1/audit/actions"));
   printf("  allowed_routes: ok\n");
}

static void test_trailing_slash_tolerated(void)
{
   assert(kb_route_acl_console_admin_allows("GET", "/v1/enrollments/"));
   assert(kb_route_acl_console_admin_allows("POST", "/v1/enrollments/abc/revoke/"));
   printf("  trailing_slash: ok\n");
}

static void test_wrong_method_denied(void)
{
   assert(!kb_route_acl_console_admin_allows("DELETE", "/v1/enrollments/abc/revoke"));
   assert(!kb_route_acl_console_admin_allows("POST", "/v1/console/overview"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/decisions/42/supersede"));
   assert(!kb_route_acl_console_admin_allows("get", "/v1/enrollments")); /* case-sensitive */
   printf("  wrong_method: ok\n");
}

static void test_sibling_and_extra_segments_denied(void)
{
   /* Not in the allowlist at all. */
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/enroll")); /* only POST is allowed */
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/review"));
   assert(!kb_route_acl_console_admin_allows("POST", "/v1/review/7/accept"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/ingest/status"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/memory"));
   /* Extra trailing segments must not widen a matched prefix. */
   assert(!kb_route_acl_console_admin_allows("POST", "/v1/enrollments/abc/revoke/extra"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/enrollments/abc"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/decisions/42/extra"));
   /* Missing the required {id} segment. */
   assert(!kb_route_acl_console_admin_allows("POST", "/v1/enrollments//revoke"));
   assert(!kb_route_acl_console_admin_allows("POST", "/v1/enrollments/revoke"));
   printf("  sibling_extra: ok\n");
}

static void test_encoded_and_malformed_denied(void)
{
   /* Percent-encoded literal segments must not match the decoded literal. */
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/%65nrollments"));
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/console/overview%00"));
   /* Query strings should never reach this function, but a stray one must deny. */
   assert(!kb_route_acl_console_admin_allows("GET", "/v1/enrollments?all=1"));
   /* Missing leading slash / NULLs. */
   assert(!kb_route_acl_console_admin_allows("GET", "v1/enrollments"));
   assert(!kb_route_acl_console_admin_allows(NULL, "/v1/enrollments"));
   assert(!kb_route_acl_console_admin_allows("GET", NULL));
   assert(!kb_route_acl_console_admin_allows("GET", ""));
   printf("  encoded_malformed: ok\n");
}

static void test_overlong_denied(void)
{
   char big[1024];
   memset(big, 'a', sizeof(big));
   big[0] = '/';
   big[sizeof(big) - 1] = '\0';
   assert(!kb_route_acl_console_admin_allows("GET", big));
   printf("  overlong: ok\n");
}

int main(void)
{
   test_allowed_routes();
   test_trailing_slash_tolerated();
   test_wrong_method_denied();
   test_sibling_and_extra_segments_denied();
   test_encoded_and_malformed_denied();
   test_overlong_denied();
   printf("test_kb_route_acl: all passed\n");
   return 0;
}

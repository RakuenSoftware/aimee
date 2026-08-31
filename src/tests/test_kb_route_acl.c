/* test_kb_route_acl.c — event-bus-only console authorization seam and the
 * separate core maintenance-family classifier. */
#include "kb_route_acl.h"

#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern aimee_module_status_t aimee_control_web_module_handler(const aimee_module_invocation_t *,
                                                              const uint8_t *, uint32_t, uint8_t *,
                                                              uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int control_web_module_provider(const char *method, const char *path, int *allowed)
{
   uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_CONTROL_WEB_STAGE_AUTHORIZE};
   if (aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, method, path,
                                        request, sizeof(request)) != 0 ||
       aimee_control_web_module_handler(&invocation, request, sizeof(request), response,
                                        sizeof(response), &response_len,
                                        NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_control_web_response_decode(response, response_len, allowed);
}

static int allows(const char *method, const char *path)
{
   int allowed = -1;
   assert(kb_route_acl_console_admin_authorize(method, path, &allowed) == 0);
   assert(allowed == 0 || allowed == 1);
   return allowed;
}

static void test_provider_required(void)
{
   int allowed = 1;
   kb_route_acl_register_authorization_provider(NULL);
   assert(kb_route_acl_console_admin_authorize("GET", "/v1/console/overview", &allowed) == -1);
   assert(allowed == 0);
   assert(kb_route_acl_console_admin_authorize("GET", "/v1/console/overview", NULL) == -1);
   kb_route_acl_register_authorization_provider(control_web_module_provider);
   printf("  provider_required: ok\n");
}

static void test_allowed_routes(void)
{
   assert(allows("GET", "/v1/console/overview"));
   assert(allows("POST", "/v1/enroll"));
   assert(allows("GET", "/v1/enrollments"));
   assert(allows("POST", "/v1/enrollments/abc123/revoke"));
   assert(allows("GET", "/v1/config/oidc"));
   assert(allows("PUT", "/v1/config/oidc"));
   assert(allows("GET", "/v1/scopes"));
   assert(allows("GET", "/v1/decisions"));
   assert(allows("GET", "/v1/decisions/42"));
   assert(allows("POST", "/v1/decisions"));
   assert(allows("POST", "/v1/decisions/42/supersede"));
   assert(allows("GET", "/v1/audit/actions"));
   assert(allows("GET", "/v1/console/pipeline"));
   assert(allows("POST", "/v1/console/pipeline/config"));
   assert(allows("GET", "/v1/console/settings"));
   assert(allows("POST", "/v1/console/settings/config"));
   assert(allows("GET", "/v1/enrollments/"));
   assert(allows("POST", "/v1/enrollments/abc/revoke/"));
   printf("  allowed_routes: ok\n");
}

static void test_denied_routes(void)
{
   static const char *const cases[][2] = {
       {"DELETE", "/v1/enrollments/abc/revoke"},
       {"POST", "/v1/console/overview"},
       {"GET", "/v1/decisions/42/supersede"},
       {"get", "/v1/enrollments"},
       {"POST", "/v1/console/pipeline"},
       {"GET", "/v1/console/pipeline/config"},
       {"POST", "/v1/console/settings"},
       {"GET", "/v1/console/settings/config"},
       {"GET", "/v1/enroll"},
       {"GET", "/v1/review"},
       {"POST", "/v1/review/7/accept"},
       {"GET", "/v1/ingest/status"},
       {"GET", "/v1/memory"},
       {"POST", "/v1/enrollments/abc/revoke/extra"},
       {"GET", "/v1/enrollments/abc"},
       {"GET", "/v1/decisions/42/extra"},
       {"POST", "/v1/enrollments//revoke"},
       {"POST", "/v1/enrollments/revoke"},
       {"GET", "/v1/%65nrollments"},
       {"GET", "/v1/console/overview%00"},
       {"GET", "/v1/enrollments?all=1"},
       {"GET", "v1/enrollments"},
       {NULL, "/v1/enrollments"},
       {"GET", NULL},
       {"GET", ""},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      assert(!allows(cases[i][0], cases[i][1]));

   char big[1024];
   memset(big, 'a', sizeof(big));
   big[0] = '/';
   big[sizeof(big) - 1] = '\0';
   int allowed = 1;
   assert(kb_route_acl_console_admin_authorize("GET", big, &allowed) == -1);
   assert(allowed == 0);
   printf("  denied_routes: ok\n");
}

static void test_maintenance_family(void)
{
   assert(kb_route_acl_is_maintenance("/v1/maintenance/purge"));
   assert(kb_route_acl_is_maintenance("/v1/maintenance/future-operation"));
   assert(!kb_route_acl_is_maintenance("/v1/maintenance"));
   assert(!kb_route_acl_is_maintenance("/v1/console/overview"));
   assert(!kb_route_acl_is_maintenance(NULL));
   printf("  maintenance_family: ok\n");
}

int main(void)
{
   test_provider_required();
   test_allowed_routes();
   test_denied_routes();
   test_maintenance_family();
   printf("test_kb_route_acl: all passed\n");
   return 0;
}

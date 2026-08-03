#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <string.h>

struct acl_entry
{
   const char *method;
   const char *pattern;
};

static const struct acl_entry CONSOLE_ADMIN_ACL[] = {
    {"GET", "/v1/console/overview"},
    {"GET", "/v1/console/typed_facts"},
    {"POST", "/v1/console/typed_facts/config"},
    {"POST", "/v1/console/typed_facts/relation"},
    {"GET", "/v1/console/pipeline"},
    {"GET", "/v1/console/settings"},
    {"POST", "/v1/console/settings/config"},
    {"POST", "/v1/console/pipeline/config"},
    {"POST", "/v1/enroll"},
    {"GET", "/v1/enrollments"},
    {"POST", "/v1/enrollments/{id}/revoke"},
    {"GET", "/v1/config/oidc"},
    {"PUT", "/v1/config/oidc"},
    {"GET", "/v1/scopes"},
    {"GET", "/v1/decisions"},
    {"GET", "/v1/decisions/{id}"},
    {"POST", "/v1/decisions"},
    {"POST", "/v1/decisions/{id}/supersede"},
    {"POST", "/v1/decisions/{id}/outcome"},
    {"POST", "/v1/decisions/{id}/status"},
    {"POST", "/v1/decisions/{id}/revisit"},
    {"GET", "/v1/audit/actions"},
};

static const struct acl_entry FLEET_ACL[] = {
    {"GET", "/v1/servers"},
    {"GET", "/v1/servers/{id}/health"},
    {"GET", "/v1/servers/{id}/agents"},
    {"GET", "/v1/servers/{id}/config"},
    {"POST", "/v1/servers/{id}/actions"},
};

static int segment_matches(const char *pattern, size_t pattern_len, const char *segment,
                           size_t segment_len)
{
   if (pattern_len == 4 && memcmp(pattern, "{id}", 4) == 0)
      return segment_len > 0;
   return pattern_len == segment_len && memcmp(pattern, segment, segment_len) == 0;
}

static int path_matches(const char *pattern, const char *path)
{
   const char *pattern_cursor = pattern, *path_cursor = path;
   for (;;)
   {
      if (*pattern_cursor != '/' || *path_cursor != '/')
         return 0;
      pattern_cursor++;
      path_cursor++;
      const char *pattern_segment = pattern_cursor, *path_segment = path_cursor;
      while (*pattern_cursor && *pattern_cursor != '/')
         pattern_cursor++;
      while (*path_cursor && *path_cursor != '/')
         path_cursor++;
      if (!segment_matches(pattern_segment, (size_t)(pattern_cursor - pattern_segment),
                           path_segment, (size_t)(path_cursor - path_segment)))
         return 0;
      int pattern_end = *pattern_cursor == '\0', path_end = *path_cursor == '\0';
      if (pattern_end && path_end)
         return 1;
      if (pattern_end != path_end)
         return 0;
   }
}

static int acl_allows(const struct acl_entry *entries, size_t count, const char *method,
                      const char *path, int tolerate_trailing_slash)
{
   if (!method[0] || path[0] != '/')
      return 0;
   size_t path_len = strlen(path);
   if (path_len >= AIMEE_CONTROL_WEB_PATH_MAX + 1u)
      return 0;
   char normalized[AIMEE_CONTROL_WEB_PATH_MAX + 1u];
   if (path_len > 1 && path[path_len - 1] == '/')
   {
      if (!tolerate_trailing_slash)
         return 0;
      path_len--;
   }
   memcpy(normalized, path, path_len);
   normalized[path_len] = '\0';
   for (size_t i = 0; i < count; ++i)
      if (strcmp(method, entries[i].method) == 0 && path_matches(entries[i].pattern, normalized))
         return 1;
   return 0;
}

static int proxy_route_allowed(aimee_control_web_target_t target, const char *method,
                               const char *path)
{
   if (target == AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN)
      return acl_allows(CONSOLE_ADMIN_ACL,
                        sizeof(CONSOLE_ADMIN_ACL) / sizeof(CONSOLE_ADMIN_ACL[0]), method, path, 1);
   return acl_allows(FLEET_ACL, sizeof(FLEET_ACL) / sizeof(FLEET_ACL[0]), method, path, 0);
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   aimee_control_web_target_t target;
   char method[AIMEE_CONTROL_WEB_METHOD_MAX + 1u];
   char path[AIMEE_CONTROL_WEB_PATH_MAX + 1u];
   if (!invocation || !response_len ||
       invocation->stage_id != AIMEE_CONTROL_WEB_STAGE_AUTHORIZE ||
       response_capacity < AIMEE_CONTROL_WEB_RESPONSE_LEN ||
       aimee_control_web_request_decode(request_body, request_len, &target, method, path) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_control_web_response_encode(proxy_route_allowed(target, method, path), response_body,
                                         response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_CONTROL_WEB_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

#include "server_http_internal.h"
#include <aimee/ir/session_event.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
/* The optional module owns command semantics. HTTP supplies only verified
 * request context and bounded bytes, through the same hook as the KB adapter. */
#include "kb_http_console.h"
extern cJSON *aimee_module_host_command_context(void) __attribute__((weak));
const unsigned aimee_console_http_adapter_contract_v1
    __attribute__((used, externally_visible, retain)) = 1;
struct module_console_http_scope
{
   cJSON *context;
   char *context_json, *body;
};
static void module_console_http_cleanup(void *opaque)
{
   struct module_console_http_scope *scope = opaque;
   cJSON_Delete(scope->context);
   free(scope->context_json);
   free(scope->body);
}
int rh_module_console_admin(const route_req_t *rq, char *resp, int cap)
{
   if (!rq || !resp || cap < 3)
      return 500;
   if (!aimee_module_console_admin_extension || !aimee_module_host_command_context)
      return err_json(resp, cap, 503, "console module unavailable");
   if (!rq->body || rq->body_len <= 0 || rq->body_len > 1024 * 1024 ||
       memchr(rq->body, '\0', (size_t)rq->body_len))
      return err_json(resp, cap, 400, "bounded JSON object required");
   cJSON *body = aimee_ir_json_parse_frame(rq->body, (size_t)rq->body_len);
   int valid = cJSON_IsObject(body);
   cJSON_Delete(body);
   if (!valid)
      return err_json(resp, cap, 400, "bounded JSON object required");
   struct module_console_http_scope scope = {0};
   volatile int result = 503;
   pthread_cleanup_push(module_console_http_cleanup, &scope);
   scope.context = aimee_module_host_command_context();
   const cJSON *auth = cJSON_GetObjectItemCaseSensitive(scope.context, "authenticated"),
               *principal = cJSON_GetObjectItemCaseSensitive(scope.context, "principal");
   if (!cJSON_IsObject(scope.context) || !cJSON_IsTrue(auth) || !cJSON_IsString(principal) ||
       !principal->valuestring[0])
      result = err_json(resp, cap, 403, "verified console identity required");
   else
   {
      scope.context_json = cJSON_PrintUnformatted(scope.context);
      scope.body = malloc((size_t)rq->body_len + 1);
      if (!scope.context_json || strlen(scope.context_json) > 4096 || !scope.body)
         result = err_json(resp, cap, 503, "console context unavailable");
      else
      {
         memcpy(scope.body, rq->body, (size_t)rq->body_len);
         scope.body[rq->body_len] = 0;
         memset(resp, 0, (size_t)cap);
         result = aimee_module_console_admin_extension(rq->method, rq->path, scope.body,
                                                       scope.context_json, resp, cap);
         size_t length = strnlen(resp, (size_t)cap);
         cJSON *answer = length < (size_t)cap ? aimee_ir_json_parse_frame(resp, length) : NULL;
         valid = result >= 200 && result <= 599 && cJSON_IsObject(answer);
         cJSON_Delete(answer);
         if (!valid)
            result = err_json(resp, cap, 503, "console module response unavailable");
      }
   }
   pthread_cleanup_pop(1);
   return result;
}

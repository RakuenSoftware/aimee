#include "server_http_internal.h"
#include "owned_user_source.h"
#include "host_original_text.h"
#include <aimee/ir/session_event.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
struct owned_user_source_scope
{
   char *frame;
   size_t length;
   void *purpose;
};
static void owned_user_source_cleanup(void *pointer)
{
   struct owned_user_source_scope *scope = pointer;
   if (aimee_module_owned_user_source_release_v1)
      aimee_module_owned_user_source_release_v1(scope->purpose);
   if (scope->frame && scope->length <= HOST_TEXT_MAX_INPUT + HOST_TEXT_MAX_SIDECARS + 8192u)
   {
      volatile char *bytes = scope->frame;
      for (size_t i = 0; i < scope->length; ++i)
         bytes[i] = 0;
   }
   free(scope->frame);
}
/* Generic terminal action. No operation, identity, source or module policy
 * is parsed by the host. Normal source-only preparation stays byte-compatible
 * when the optional module callback declines this request. */
struct owned_user_action_scope
{
   char *result;
   size_t length, capacity;
};
static void owned_user_action_cleanup(void *pointer)
{
   struct owned_user_action_scope *scope = pointer;
   if (scope->result && scope->capacity <= 16385)
   {
      volatile char *bytes = scope->result;
      for (size_t i = 0; i < scope->capacity; ++i)
         bytes[i] = 0;
   }
   free(scope->result);
}
static int handle_owned_user_action(int fd, const char *headers, const char *body,
                                    size_t body_length, const char *request_id)
{
   if (!aimee_module_owned_user_action_v1)
      return 0;
   struct owned_user_action_scope scope = {0};
   volatile int handled = 1;
   pthread_cleanup_push(owned_user_action_cleanup, &scope);
   int status = aimee_module_owned_user_action_v1(headers, body, body_length, &scope.result,
                                                  &scope.length, &scope.capacity);
   if (status == 0 && !scope.result && !scope.length && !scope.capacity)
      handled = 0;
   else
   {
      cJSON *data = NULL;
      if ((status == 1 || status == 2) && scope.result && scope.length && scope.length <= 16384 &&
          scope.capacity == scope.length + 1 &&
          memchr(scope.result, 0, scope.capacity) == scope.result + scope.length)
         data = aimee_ir_json_parse_frame(scope.result, scope.length);
      if (cJSON_IsObject(data))
         send_response(fd, 200, scope.result, request_id);
      else
         send_response(fd, 503, "{\"error\":\"owned action unavailable\"}", request_id);
      cJSON_Delete(data);
   }
   pthread_cleanup_pop(1);
   return handled;
}
__attribute__((used, externally_visible, retain))
const unsigned aimee_owned_user_action_handoff_contract_v1 = 1;

void server_http_owned_user_source(int fd, const char *headers, const char *body,
                                   size_t body_length, uint32_t conn_caps, const char *request_id,
                                   server_http_owned_stream_fn forward)
{
   if (handle_owned_user_action(fd, headers, body, body_length, request_id))
      return;
   if (!aimee_module_owned_user_source_prepare_v1 || !aimee_module_owned_user_source_release_v1)
   {
      send_response(fd, 503, "{\"error\":\"owned user input unavailable\"}", request_id);
      return;
   }
   struct owned_user_source_scope scope = {0};
   pthread_cleanup_push(owned_user_source_cleanup, &scope);
   int prepared = aimee_module_owned_user_source_prepare_v1(
       headers, body, body_length, &scope.frame, &scope.length, &scope.purpose);
   if (prepared || !scope.purpose || !scope.frame || !scope.length ||
       scope.length > HOST_TEXT_MAX_INPUT + HOST_TEXT_MAX_SIDECARS + 8192u ||
       memchr(scope.frame, 0, scope.length + 1) != scope.frame + scope.length)
      send_response(fd, 503, "{\"error\":\"owned user input unavailable\"}", request_id);
   else
      forward(fd, headers, scope.frame, scope.length, conn_caps, request_id, 1);
   pthread_cleanup_pop(1);
}
__attribute__((used, externally_visible, retain))
const unsigned aimee_owned_user_source_handoff_contract_v1 = 1;

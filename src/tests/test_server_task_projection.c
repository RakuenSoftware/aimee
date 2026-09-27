#include "aimee.h"
#include "server.h"
#include "request_context.h"
#include "db1_client/session_state.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *principal = "alice";
static int calls, serves, admissions, finishes;
const char *request_context_principal(void)
{
   return principal;
}
cJSON *server_error_kind_json(const char *kind, const char *message, const char *id)
{
   (void)message;
   (void)id;
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "kind", kind);
   return r;
}
int send_and_free(server_conn_t *conn, cJSON *r)
{
   (void)conn;
   cJSON_Delete(r);
   return 0;
}
cJSON *memory_serve_command(cJSON *binding)
{
   assert(!strcmp(jo_cstr(binding, "project"), "owned"));
   serves++;
   return cJSON_Parse("{\"fresh_owner\":true}");
}
cJSON *memory_task_promotion_command(const cJSON *proof)
{
   assert(!strcmp(jo_cstr(proof, "preview_digest"), "digest"));
   admissions++;
   return cJSON_Parse("{\"status\":\"ok\",\"admission\":\"review_required\"}");
}
int db1_session_task_projection_apply(const char *owner, const char *sid, const char *wire,
                                      char *reply, size_t cap)
{
   assert(!strcmp(owner, "alice") && !strcmp(sid, "session"));
   cJSON *r = cJSON_Parse(wire);
   assert(r && !cJSON_HasObjectItem(r, "authoritative"));
   const char *op = jo_cstr(r, "operation");
   const char *answer = "{\"status\":\"ok\"}";
   if (!strcmp(op, "describe"))
      answer = "{\"status\":\"ok\",\"projection\":{\"binding\":{\"project\":\"owned\"}}}";
   else if (!strcmp(op, "promotion_finish"))
   {
      finishes++;
      assert(!strcmp(jo_cstr(cJSON_GetObjectItem(r, "promotion_result"), "admission"),
                     "review_required"));
   }
   else
   {
      const cJSON *prepared = cJSON_GetObjectItem(r, "prepared");
      assert(cJSON_IsTrue(cJSON_GetObjectItem(prepared, "fresh_owner")));
      assert(!cJSON_HasObjectItem(prepared, "forged"));
      assert(!cJSON_HasObjectItem(r, "promotion_result"));
      if (!strcmp(op, "promote"))
         answer = "{\"status\":\"ok\",\"preview_digest\":\"digest\"}";
   }
   snprintf(reply, cap, "%s", answer);
   cJSON_Delete(r);
   calls++;
   return 0;
}
int main(void)
{
   cJSON *r =
       cJSON_Parse("{\"operation\":\"rebuild\",\"session_id\":\"session\",\"task_id\":\"1\","
                   "\"binding\":{\"project\":\"owned\"},\"prepared\":{\"forged\":true},\"promotion_"
                   "result\":{\"admission\":\"forged\"},\"authoritative\":true}");
   principal = NULL;
   cJSON *result = task_projection_command(r);
   assert(!strcmp(jo_cstr(result, "kind"), "invalid_argument") && calls == 0);
   cJSON_Delete(result);
   principal = "alice";
   result = task_projection_command(r);
   cJSON_Delete(result);
   assert(calls == 1 && serves == 1);
   cJSON_ReplaceItemInObject(r, "operation", cJSON_CreateString("get"));
   result = task_projection_command(r);
   cJSON_Delete(result);
   assert(calls == 3 && serves == 2);
   cJSON_ReplaceItemInObject(r, "operation", cJSON_CreateString("promote"));
   result = task_projection_command(r);
   assert(!strcmp(jo_cstr(result, "admission"), "review_required"));
   cJSON_Delete(result);
   assert(admissions == 1 && finishes == 1 && calls == 6);
   cJSON_ReplaceItemInObject(r, "operation", cJSON_CreateString("promotion_finish"));
   result = task_projection_command(r);
   assert(!strcmp(jo_cstr(result, "kind"), "invalid_argument") && calls == 6);
   cJSON_Delete(result);
   cJSON_Delete(r);
   puts("task projection transport boundaries passed");
   return 0;
}

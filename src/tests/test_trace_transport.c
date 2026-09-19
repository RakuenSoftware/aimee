/* The remaining native trace connection transports exact IDs and host context.
 * Go PostgreSQL replay owns pattern/persistence/cursor transaction coverage. */
#include "aimee.h"
#include "cJSON.h"
#include "db1_client/execution_trace.h"
#include "json_fluent.h"
#include "trace_analysis.h"
#include <assert.h>

static int load_count = 2, unavailable, applies, reads;
static const int64_t last_id = INT64_C(9007199254741507);
static const char *state_receipt = "{\"status\":\"ok\",\"last_id\":\"9007199254741507\"}";
static const char *apply_receipt =
    "{\"status\":\"ok\",\"last_id\":\"9007199254741509\",\"emitted\":1}";
const char *session_id(void)
{
   return "trace-session";
}

int db1_execution_trace_list_after_id(int64_t after, db1_execution_trace_mining_row_t *out, int max)
{
   assert(after == last_id && max >= 2);
   reads++;
   for (int i = 0; i < load_count; i++)
   {
      out[i].id = last_id + i + 1;
      out[i].plan_id = 7;
      out[i].turn = i + 1;
      snprintf(out[i].tool_name, sizeof(out[i].tool_name), "%s", i == 0 ? "Read" : "Search");
      snprintf(out[i].tool_result, sizeof(out[i].tool_result), "%s", i == 0 ? "error" : "ok");
   }
   return load_count;
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **response)
{
   assert(!strcmp(method, "memory.runtime"));
   if (unavailable)
   {
      *response = NULL;
      return -1;
   }
   if (!strcmp(jo_cstr(args, "operation"), "trace-state"))
   {
      *response = cJSON_Parse(state_receipt);
      return 1;
   }
   assert(!strcmp(jo_cstr(args, "operation"), "trace-apply"));
   assert(!strcmp(jo_cstr(args, "session_id"), "trace-session"));
   assert(!cJSON_HasObjectItem(args, "project"));
   const cJSON *batch = cJSON_GetObjectItemCaseSensitive(args, "batch");
   assert(!strcmp(jo_cstr(batch, "after_id"), "9007199254741507"));
   const cJSON *rows = cJSON_GetObjectItemCaseSensitive(batch, "rows");
   assert(cJSON_GetArraySize(rows) == 2);
   for (int i = 0; i < 2; i++)
   {
      const cJSON *row = cJSON_GetArrayItem(rows, i);
      assert(!strcmp(jo_cstr(row, "id"), i == 0 ? "9007199254741508" : "9007199254741509"));
      assert(cJSON_GetObjectItemCaseSensitive(row, "turn")->valueint == i + 1);
      assert(!strcmp(jo_cstr(row, "tool_result"), i == 0 ? "error" : "ok"));
   }
   applies++;
   *response = cJSON_Parse(apply_receipt);
   return 1;
}
int main(void)
{
   assert(trace_mine() == 1 && applies == 1 && reads == 1);
   unavailable = 1;
   assert(trace_mine() == -1 && applies == 1 && reads == 1);
   unavailable = 0;
   const char *bad_state[] = {"{}",
                              "{\"status\":\"error\"}",
                              "{\"status\":\"ok\",\"last_id\":9007199254741507}",
                              "{\"status\":\"ok\",\"last_id\":\"-1\"}",
                              "{\"status\":\"ok\",\"last_id\":\"01\"}",
                              "{\"status\":\"ok\",\"last_id\":\"9223372036854775808\"}"};
   for (size_t i = 0; i < sizeof(bad_state) / sizeof(bad_state[0]); i++)
   {
      state_receipt = bad_state[i];
      assert(trace_mine() == -1 && applies == 1 && reads == 1);
   }
   state_receipt = "{\"status\":\"ok\",\"last_id\":\"9007199254741507\"}";
   const char *bad_apply[] = {
       "{}", "{\"status\":\"error\"}",
       "{\"status\":\"ok\",\"last_id\":\"9007199254741509\",\"emitted\":1.5}",
       "{\"status\":\"ok\",\"last_id\":\"9007199254741509\",\"emitted\":-1}",
       "{\"status\":\"ok\",\"last_id\":\"9007199254741508\",\"emitted\":1}"};
   for (size_t i = 0; i < sizeof(bad_apply) / sizeof(bad_apply[0]); i++)
   {
      apply_receipt = bad_apply[i];
      assert(trace_mine() == -1);
   }
   int before = applies;
   load_count = -1;
   assert(trace_mine() == -1 && applies == before);
   load_count = 0;
   assert(trace_mine() == 0 && applies == before);
   puts("trace connection: exact IDs, context and malformed/error responses passed");
   return 0;
}

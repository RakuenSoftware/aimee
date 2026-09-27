/* Exercise the shipping adapter: ownership, refusal and exact JSON transport. */
#include <assert.h>
#include "../kb/db2_adapters/kb_service_backend_agent.c"

static const char *expected_operation;
static const char *owner_json;
static int owner_rc = 1;
static int with_context = 1;

cJSON *kb_service_command_context(void)
{
   return with_context ? cJSON_Parse("{\"authenticated\":true}") : NULL;
}

int aimee_module_commands_dispatch_internal_context_timeout(const char *method, const cJSON *args,
                                                            const cJSON *context, int timeout_ms,
                                                            cJSON **result)
{
   assert(!strcmp(method, "memory.runtime"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation")),
                  expected_operation));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(context, "authenticated")));
   assert(timeout_ms == 60000);
   if (!strcmp(expected_operation, "rules-export"))
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path")),
                     "/tmp/rules transport.jsonl"));
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   if (owner_json)
      cJSON_AddStringToObject(*result, "json", owner_json);
   return owner_rc;
}

/* The adapter must never use these raw storage readers on success or failure. */
int db2_rules_list(rule_t *out, int limit)
{
   (void)out;
   (void)limit;
   assert(!"unfiltered rule storage read");
   return -1;
}
char *db2_rules_generate(void)
{
   assert(!"unfiltered rule markdown");
   return NULL;
}
int db2_rules_export_jsonl(const char *path)
{
   (void)path;
   assert(!"unfiltered rule export");
   return -1;
}

static void exact(cJSON *reply)
{
   assert(reply);
   char *raw = cJSON_PrintUnformatted(reply);
   assert(raw && !strcmp(raw, owner_json));
   free(raw);
   cJSON_Delete(reply);
}

int main(void)
{
   expected_operation = "rules-list";
   owner_json = "{\"status\":\"ok\",\"rules\":[{\"id\":9007199254740993,\"title\":\"exact\"}]}";
   exact(db2_kb_service_rules_list_json(16));
   const char *bad[] = {NULL, "{}", "{\"status\":\"ok\",\"rules\":7}",
                        "{\"status\":\"ok\"} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      owner_json = bad[i];
      assert(!db2_kb_service_rules_list_json(16));
   }
   owner_json = "{\"status\":\"error\",\"kind\":\"unavailable\"}";
   exact(db2_kb_service_rules_list_json(16));
   owner_rc = -1;
   assert(!db2_kb_service_rules_list_json(16));
   owner_rc = 1;
   with_context = 0;
   assert(!db2_kb_service_rules_list_json(16));
   with_context = 1;
   expected_operation = "rules-generate";
   owner_json = "{\"status\":\"ok\",\"content\":\"# Rules\\n\\n\"}";
   exact(db2_kb_service_rules_generate_json());
   owner_json = "{\"status\":\"ok\",\"content\":7}";
   assert(!db2_kb_service_rules_generate_json());
   expected_operation = "rules-export";
   owner_json = "{\"status\":\"ok\",\"count\":0}";
   exact(db2_kb_service_rules_export_jsonl_json("/tmp/rules transport.jsonl"));
   owner_json = "{\"status\":\"ok\",\"count\":-1}";
   assert(!db2_kb_service_rules_export_jsonl_json("/tmp/rules transport.jsonl"));
   return 0;
}

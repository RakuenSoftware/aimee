/* Actual data CLI: export failures and unavailable duplicate checks are fatal. */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "log.h"
#include "platform_test_util.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *export_reply = "{\"status\":\"ok\",\"count\":2}", *lookup_reply;
static int insert_count, insert_failure, expect_no_insert;
static char directory[256];
void fatal(const char *fmt, ...)
{
   (void)fmt;
   if (expect_no_insert)
      assert(insert_count == 0);
   _exit(17);
}
void now_utc(char *buf, size_t n)
{
   snprintf(buf, n, "2026-09-18T00:00:00Z");
}
int platform_mkdir_p(const char *path, int mode)
{
   (void)mode;
   assert(!strcmp(path, directory));
   return 0;
}
int config_present(void)
{
   return 0;
}
const char *config_guardrail_mode(void)
{
   return "enforce";
}
int kb_client_rules_export_jsonl(const char *path)
{
   (void)path;
   assert(0);
   return -1;
}
int kb_client_rules_insert(const char *p, const char *t, const char *d, int w)
{
   (void)p;
   (void)t;
   (void)d;
   (void)w;
   assert(0);
   return -1;
}
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
void kb_client_memory_scope_context_apply(cJSON *args)
{
   cJSON_AddBoolToObject(args, "scope_context", 1);
   cJSON_AddStringToObject(args, "project", "data-test");
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(jo_bool(args, "scope_context", 0));
   assert(!strcmp(jo_cstr(args, "project"), "data-test"));
   const char *reply = export_reply;
   if (!strcmp(method, "memory.key_exists"))
   {
      reply = lookup_reply                                ? lookup_reply
              : !strcmp(jo_cstr(args, "key"), "existing") ? "{\"status\":\"ok\",\"exists\":true}"
                                                          : "{\"status\":\"ok\",\"exists\":false}";
   }
   else
   {
      assert(!strcmp(method, "memory.export_jsonl") ||
             !strcmp(method, "memory.decisions_export_jsonl"));
      assert(strstr(jo_cstr(args, "path"), directory) == jo_cstr(args, "path"));
   }
   cJSON_Delete(args);
   return reply ? strdup(reply) : NULL;
}
int kb_client_memory_insert(const char *tier, const char *kind, const char *key,
                            const char *content, double confidence, const char *session,
                            memory_t *out)
{
   (void)out;
   assert(!strcmp(tier, "L2") && !strcmp(kind, "fact") && !strcmp(key, "fresh"));
   assert(!strcmp(content, "new text") && confidence == 1 && !strcmp(session, "import"));
   insert_count++;
   return insert_failure ? -1 : 0;
}
static void run(int importing, const char *category)
{
   char output[300], selection[128];
   snprintf(output, sizeof(output), "--output=%s", directory);
   snprintf(selection, sizeof(selection), "--category=%s", category);
   char *args[] = {importing ? directory : output, selection};
   if (importing)
      cmd_import(NULL, 2, args);
   else
      cmd_export(NULL, 2, args);
}
static void rejected(int importing, const char *category)
{
   fflush(NULL);
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      FILE *null = freopen("/dev/null", "w", stdout);
      assert(null);
      run(importing, category);
      _exit(0);
   }
   int status;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 17);
}
int main(void)
{
   snprintf(directory, sizeof(directory), "%s/aimee-data-consumer-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory));
   run(0, "memories");
   run(0, "decisions");
   const char *bad_exports[] = {NULL,
                                "bad-json",
                                "{\"status\":\"error\",\"message\":\"refused\"}",
                                "{\"status\":\"ok\"}",
                                "{\"status\":\"ok\",\"count\":-1}",
                                "{\"status\":\"ok\",\"count\":1.5}"};
   expect_no_insert = 1;
   for (size_t i = 0; i < sizeof(bad_exports) / sizeof(bad_exports[0]); i++)
   {
      export_reply = bad_exports[i];
      rejected(0, "memories");
      rejected(0, "decisions");
   }
   char path[320];
   snprintf(path, sizeof(path), "%s/memories.jsonl", directory);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("{\"key\":\"existing\",\"content\":\"old text\"}\n{\"key\":\"fresh\",\"content\":\"new "
         "text\"}\n",
         f);
   fclose(f);
   const char *bad_lookups[] = {"bad-json", "{\"status\":\"error\",\"message\":\"unavailable\"}",
                                "{\"status\":\"ok\"}", "{\"status\":\"ok\",\"exists\":\"false\"}"};
   for (size_t i = 0; i < sizeof(bad_lookups) / sizeof(bad_lookups[0]); i++)
   {
      lookup_reply = bad_lookups[i];
      rejected(1, "memories");
   }
   lookup_reply = NULL;
   expect_no_insert = 0;
   insert_failure = 1;
   rejected(1, "memories");
   insert_failure = 0;
   run(1, "memories");
   assert(insert_count == 1);
   unlink(path);
   snprintf(path, sizeof(path), "%s/manifest.json", directory);
   unlink(path);
   rmdir(directory);
   puts("data CLI memory commands: ok");
   return 0;
}

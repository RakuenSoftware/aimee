/* Checkpoints retain DB1 storage and use the shared Go memory owner. */
#include "aimee.h"
#include "commands.h"
#include "tasks_compose.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

void mem_checkpoint(app_ctx_t *, int, char **);
static const char *forced;
static int unavailable, inserts, task_failure, decision_failure;
static db1_checkpoint_t saved;
static int gets;
static char fact_text[9001];

void kb_client_memory_scope_context_apply(cJSON *args)
{
   cJSON_AddBoolToObject(args, "scope_context", 1);
   cJSON_AddStringToObject(args, "project", "checkpoint-project");
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strcmp(method, "memory.checkpoint"));
   assert(jo_bool(args, "scope_context", 0));
   assert(!strcmp(jo_cstr(args, "project"), "checkpoint-project"));
   int restore = !strcmp(jo_cstr(args, "action"), "restore");
   if (restore)
   {
      assert(!strcmp(jo_cstr(args, "checkpoint_id"), "9223372036854775807"));
      assert(!strcmp(jo_cstr(args, "snapshot"), saved.snapshot));
      assert(!strcmp(jo_cstr(args, "session_id"), "restore-session"));
      assert(!cJSON_HasObjectItem(args, "tier") && !cJSON_HasObjectItem(args, "key"));
   }
   else
      assert(!strcmp(jo_cstr(args, "action"), "facts"));
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   if (restore)
      return strdup("{\"status\":\"ok\",\"memory_id\":\"9223372036854775806\"}");
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON *facts = cJSON_AddArrayToObject(out, "facts");
   cJSON *fact = cJSON_CreateObject();
   cJSON_AddStringToObject(fact, "key", "fact-key");
   cJSON_AddStringToObject(fact, "content", fact_text);
   cJSON_AddItemToArray(facts, fact);
   char *result = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return result;
}
int kb_client_task_list(const char *state, const char *session, int limit, aimee_task_t *out,
                        int max)
{
   assert(!strcmp(state, TASK_IN_PROGRESS) && !session && limit == 32 && max == 32);
   if (task_failure)
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = 9;
   strcpy(out->title, "task");
   strcpy(out->state, TASK_IN_PROGRESS);
   return 1;
}
int kb_client_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out,
                                int max)
{
   assert(!outcome && limit == 8 && max == 8);
   if (decision_failure)
      return -1;
   memset(out, 0, sizeof(*out));
   strcpy(out->chosen, "choice");
   strcpy(out->rationale, "reason");
   return 1;
}
int db1_checkpoint_insert(const char *label, const char *session, int64_t task,
                          const char *snapshot, db1_checkpoint_t *out)
{
   assert(!strcmp(label, "label") && !strcmp(session, "source-session") && task == 42);
   assert(strlen(snapshot) < sizeof(out->snapshot));
   strcpy(saved.snapshot, snapshot);
   *out = saved;
   inserts++;
   return 0;
}
int db1_checkpoint_get(int64_t id, db1_checkpoint_t *out)
{
   assert(id == INT64_MAX);
   gets++;
   *out = saved;
   return 0;
}
int db1_checkpoint_list(int limit, db1_checkpoint_t *out, int max)
{
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}
int db1_checkpoint_delete(int64_t id)
{
   (void)id;
   return 0;
}
cJSON *checkpoint_to_json(const db1_checkpoint_t *cp)
{
   (void)cp;
   return cJSON_CreateObject();
}
void emit_json_ctx(cJSON *json, const char *fields, const char *profile)
{
   (void)fields;
   (void)profile;
   cJSON_Delete(json);
}
void emit_ok_ctx(const char *fields, const char *profile)
{
   (void)fields;
   (void)profile;
}

static void restore_cli(void)
{
   app_ctx_t ctx = {0};
   char *args[] = {"restore", "9223372036854775807", "--session", "restore-session"};
   mem_checkpoint(&ctx, 4, args);
}
int main(void)
{
   memset(fact_text, 'x', 4000);
   db1_checkpoint_t cp;
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == 0);
   assert(inserts == 1);
   cJSON *snapshot = cJSON_Parse(cp.snapshot);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(snapshot, "tasks")) == 1);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(snapshot, "decisions")) == 1);
   cJSON *fact = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(snapshot, "facts"), 0);
   assert(!strcmp(jo_cstr(fact, "content"), fact_text));
   cJSON_Delete(snapshot);
   assert(tasks_checkpoint_restore(INT64_MAX, "restore-session") == 0 && gets == 1);
   restore_cli();
   const char *errors[] = {"bad-json", "{}", "{\"status\":\"ok\"}", "{\"status\":\"error\"}"};
   for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
   {
      forced = errors[i];
      assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
      assert(tasks_checkpoint_restore(INT64_MAX, "restore-session") == -1);
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         restore_cli();
         _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 1);
   }
   forced = "{\"status\":\"ok\",\"facts\":null}";
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
   forced = NULL;
   unavailable = 1;
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
   assert(tasks_checkpoint_restore(INT64_MAX, "restore-session") == -1);
   unavailable = 0;
   task_failure = 1;
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
   task_failure = 0;
   decision_failure = 1;
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
   decision_failure = 0;
   memset(fact_text, 'x', sizeof(fact_text) - 1);
   assert(tasks_checkpoint_create("label", "source-session", 42, &cp) == -1);
   assert(inserts == 1); /* No partial/oversized snapshots persisted. */
   return 0;
}

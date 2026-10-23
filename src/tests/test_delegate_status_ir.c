/* Public delegate.status must project the authenticated current canonical
 * attempt, never the job's legacy display or a previous attempt's messages. */
#include "request_context.h"
#include <aimee/ir/session_event.h>
int db1_agent_job_canonical_observation(int, const char *, char **);
#include "../server/server_delegate_status.c"
#include "../server/server_jobs_aux.c"
#include <assert.h>

static request_context_t owner;
static cJSON *captured, *owned_observation;
static int observation_rc, query_calls, get_calls, frees;
static const char *malformed_wire;
static const char *job_status = "done", *legacy = "POISONED FLAT RESULT";

const request_context_t *request_context_get(void)
{
   return &owner;
}
int db1_agent_job_get(int id, db1_agent_job_t *job)
{
   assert(id == 7);
   get_calls++;
   memset(job, 0, sizeof *job);
   job->id = id;
   snprintf(job->status, sizeof job->status, "%s", job_status);
   snprintf(job->role, sizeof job->role, "reviewer");
   snprintf(job->agent_name, sizeof job->agent_name, "native-seat");
   job->cursor_turn = 9;
   job->result = strdup(legacy);
   job->prompt = strdup("business input");
   assert(job->result && job->prompt);
   return 0;
}
void db1_agent_job_update(int id, const char *status, int cursor, const char *result)
{
   (void)id;
   (void)status;
   (void)cursor;
   (void)result;
   assert(!"delegate.status must never write job state");
}
void db1_agent_job_free(db1_agent_job_t *job)
{
   frees++;
   free(job->prompt);
   free(job->result);
   memset(job, 0, sizeof *job);
}
int db1_agent_job_canonical_observation(int id, const char *actor, char **out)
{
   assert(id == 7 && !strcmp(actor, "webuser:owner"));
   query_calls++;
   *out = observation_rc == 0 ? (malformed_wire ? strdup(malformed_wire)
                                                : cJSON_PrintUnformatted(owned_observation))
                              : NULL;
   return observation_rc;
}
int delegate_default_max_turns_for_role(const char *role)
{
   (void)role;
   return 10;
}
int delegate_final_after_turns_for_role(const char *role)
{
   (void)role;
   return 8;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   cJSON_Delete(captured);
   captured = cJSON_Duplicate(response, 1);
   assert(captured);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *id)
{
   (void)conn;
   (void)message;
   (void)id;
   assert(!"unexpected public handler error");
   return -1;
}
static const cJSON *field(const cJSON *o, const char *name)
{
   return cJSON_GetObjectItemCaseSensitive(o, name);
}
static const char *string(const cJSON *o, const char *name)
{
   return cJSON_GetStringValue(field(o, name));
}
static void absent(const char *name)
{
   assert(!field(captured, name));
}
static void poll(int full)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 7);
   cJSON_AddStringToObject(req, "actor", "webuser:spoofed");
   cJSON_AddStringToObject(req, "session_id", "forged-session");
   if (full)
      cJSON_AddBoolToObject(req, "full_result", 1);
   int before = get_calls, old_frees = frees;
   assert(handle_delegate_status(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(get_calls == before + 1 && frees == old_frees + 1);
   assert(!strcmp(string(captured, "job_status"), observation_rc == -2 ? "forbidden" : job_status));
}
static void reset(const char *status)
{
   cJSON_Delete(owned_observation);
   owned_observation = cJSON_CreateObject();
   observation_rc = 0;
   malformed_wire = NULL;
   cJSON_AddStringToObject(owned_observation, "schema", "aimee.agent-job-observation.v1");
   cJSON_AddStringToObject(owned_observation, "job_id", "7");
   cJSON *parent = cJSON_AddObjectToObject(owned_observation, "parent_source");
   cJSON_AddStringToObject(parent, "actor", "webuser:owner");
   cJSON_AddStringToObject(parent, "session_id", "parent-session");
   cJSON *child = cJSON_AddObjectToObject(owned_observation, "child");
   cJSON_AddStringToObject(child, "session_id", "child-session");
   cJSON_AddStringToObject(child, "turn_id", "current-turn");
   cJSON_AddStringToObject(owned_observation, "last_sequence", "20");
   cJSON_AddStringToObject(owned_observation, "execution_status", status);
   cJSON_AddObjectToObject(owned_observation, "terminal_source");
   cJSON_AddArrayToObject(owned_observation, "events");
   cJSON_AddArrayToObject(owned_observation, "history");
   job_status = "done";
   legacy = "POISONED FLAT RESULT";
}
static void append_message(const char *turn, const char *actor, aimee_block_t *blocks, int count)
{
   aimee_message_t message = {.role = "assistant", .blocks = blocks, .n_blocks = count};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_MESSAGE,
                                  .message = &message,
                                  .role = "assistant",
                                  .message_id = "authored-group"};
   /* Use the REAL canonical serializer. In the durable envelope a message is
    * entry.payload.kind=message and entry.payload.payload is the message. */
   cJSON *payload = aimee_ir_session_event_json(&event);
   assert(payload && !strcmp(string(payload, "kind"), "message"));
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "turn_id", turn);
   cJSON_AddStringToObject(entry, "actor", actor);
   cJSON_AddItemToObject(entry, "payload", payload);
   cJSON_AddItemToArray((cJSON *)field(owned_observation, "events"), entry);
   cJSON_AddItemToArray((cJSON *)field(owned_observation, "history"),
                        aimee_ir_message_json(&message));
}
static void append_text(const char *turn, const char *actor, const char *text)
{
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   append_message(turn, actor, &block, 1);
}
static void require_unavailable(void)
{
   poll(1);
   assert(!strcmp(string(captured, "canonical_state"), "unavailable"));
   absent("result");
   absent("output");
   absent("outputs");
   absent("diagnostic");
   absent("history");
   absent("events");
   absent("legacy_result");
}
static void test_jobs_aux(void)
{
   reset("completed");
   append_text("previous-attempt", "webuser:owner", "stale answer");
   append_text("current-turn", "webuser:owner", "first current group");
   append_text("current-turn", "webuser:owner", "last current group");
   db1_agent_job_t job;
   assert(db1_agent_job_get(7, &job) == 0);
   cJSON *view = agent_job_to_json(&job, 1, 1);
   assert(view && !strcmp(string(view, "result"), "last current group"));
   assert(cJSON_GetArraySize(field(view, "outputs")) == 2);
   assert(cJSON_Compare(field(view, "history"), field(owned_observation, "history"), 1));
   assert(cJSON_Compare(field(view, "events"), field(owned_observation, "events"), 1));
   char *wire = cJSON_PrintUnformatted(view);
   assert(wire && !strstr(wire, legacy));
   free(wire);
   cJSON_Delete(view);
   int queries_before = query_calls;
   view = agent_job_to_json(&job, 0, 0);
   assert(query_calls == queries_before);
   assert(!field(view, "result") && !field(view, "legacy_result") && !field(view, "prompt"));
   cJSON_Delete(view);
   db1_agent_job_free(&job);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 7);
   assert(handle_jobs_logs(NULL, NULL, req) == 0);
   assert(!strcmp(string(captured, "log"), "last current group"));
   assert(!strcmp(string(captured, "log_kind"), "canonical_text"));
   assert(cJSON_GetArraySize(field(field(captured, "job"), "outputs")) == 2);

   reset("completed");
   aimee_block_t media[] = {{.type = AIMEE_BLK_IMAGE,
                             .media_type = "image/png",
                             .media_ref = "https://public.invalid/image.png"},
                            {.type = AIMEE_BLK_THINKING,
                             .thinking_id = "opaque-id",
                             .thinking_encrypted_content = "opaque-value"}};
   append_message("current-turn", "webuser:owner", media, 2);
   assert(db1_agent_job_get(7, &job) == 0);
   view = agent_job_to_json(&job, 1, 1);
   const cJSON *entry = cJSON_GetArrayItem(field(owned_observation, "events"), 0);
   assert(cJSON_Compare(field(view, "output"), field(field(entry, "payload"), "payload"), 1));
   const char *display = string(view, "result");
   assert(!display || !display[0]);
   wire = cJSON_PrintUnformatted(view);
   assert(wire && strstr(wire, "opaque-value") && !strstr(wire, legacy));
   free(wire);
   cJSON_Delete(view);
   db1_agent_job_free(&job);
   assert(handle_jobs_logs(NULL, NULL, req) == 0);
   display = string(captured, "log");
   assert(!display || !display[0]);
   assert(field(field(captured, "job"), "output"));

   reset("running");
   job_status = "running";
   append_text("previous-attempt", "webuser:owner", "STALE PREVIOUS ANSWER");
   assert(handle_jobs_logs(NULL, NULL, req) == 0);
   assert(!field(captured, "log"));
   assert(!field(field(captured, "job"), "output"));
   assert(!field(field(captured, "job"), "result"));
   assert(cJSON_GetArraySize(field(field(captured, "job"), "history")) == 1);

   reset("completed");
   observation_rc = 1;
   assert(db1_agent_job_get(7, &job) == 0);
   view = agent_job_to_json(&job, 1, 1);
   assert(!field(view, "result") && !field(view, "diagnostic"));
   assert(!strcmp(string(view, "legacy_result"), legacy));
   cJSON_Delete(view);
   db1_agent_job_free(&job);
   assert(handle_jobs_logs(NULL, NULL, req) == 0);
   assert(!strcmp(string(captured, "log_kind"), "legacy_archive"));
   assert(!strcmp(string(captured, "log"), legacy));
   assert(!field(field(captured, "job"), "result"));

   reset("failed");
   observation_rc = 1;
   job_status = "failed";
   legacy = "host refused launch before provider";
   assert(handle_jobs_logs(NULL, NULL, req) == 0);
   assert(!strcmp(string(captured, "log_kind"), "diagnostic"));
   assert(!strcmp(string(captured, "log"), legacy));
   assert(!field(field(captured, "job"), "result"));
   assert(!field(field(captured, "job"), "legacy_result"));

   for (int rc = -1; rc >= -2; rc--)
   {
      reset("completed");
      observation_rc = rc;
      job_status = "failed";
      assert(db1_agent_job_get(7, &job) == 0);
      view = agent_job_to_json(&job, 1, 1);
      assert(!field(view, "result") && !field(view, "diagnostic") && !field(view, "legacy_result"));
      if (rc == -2)
      {
         assert(!field(view, "prompt") && !field(view, "role") && !field(view, "agent_name"));
         assert(!field(view, "cursor_turn"));
      }
      cJSON_Delete(view);
      db1_agent_job_free(&job);
      assert(handle_jobs_logs(NULL, NULL, req) == 0);
      assert(!field(captured, "log"));
      assert(!field(field(captured, "job"), "result"));
      if (rc == -2)
      {
         assert(!strcmp(string(captured, "job_status"), "forbidden"));
         const cJSON *nested = field(captured, "job");
         assert(!field(nested, "prompt") && !field(nested, "role") && !field(nested, "agent_name"));
         assert(!field(nested, "cursor_turn"));
      }
   }
   cJSON_Delete(req);
}
int main(void)
{
   snprintf(owner.vault_principal, sizeof owner.vault_principal, "webuser:owner");
   reset("completed");
   append_text("previous-attempt", "webuser:owner", "STALE PREVIOUS ANSWER");
   append_text("current-turn", "webuser:owner", "first current group");
   append_text("current-turn", "webuser:owner", "last current group");
   poll(0);
   assert(!strcmp(string(captured, "result"), "last current group"));
   assert(query_calls == 1);
   assert(cJSON_GetArraySize(field(captured, "outputs")) == 2);
   absent("history");
   absent("events");
   absent("diagnostic");
   const cJSON *events = field(owned_observation, "events");
   const cJSON *last = field(field(cJSON_GetArrayItem(events, 2), "payload"), "payload");
   assert(cJSON_Compare(field(captured, "output"), last, 1));
   poll(1);
   assert(cJSON_Compare(field(captured, "events"), events, 1));
   assert(cJSON_Compare(field(captured, "history"), field(owned_observation, "history"), 1));
   /* Changing only a legacy display cannot change any canonical output. */
   cJSON *saved = cJSON_Duplicate(captured, 1);
   legacy = "COMPLETELY DIFFERENT POISON";
   poll(1);
   assert(cJSON_Compare(saved, captured, 1));
   cJSON_Delete(saved);

   /* Full media and opaque THINKING commitments remain exact canonical data;
    * neither the opaque value nor the old display becomes compatibility text. */
   reset("completed");
   aimee_block_t media[] = {{.type = AIMEE_BLK_IMAGE,
                             .media_type = "image/png",
                             .media_ref = "https://public.invalid/image.png"},
                            {.type = AIMEE_BLK_DOCUMENT,
                             .media_type = "application/pdf",
                             .media_ref = "data:application/pdf;base64,AA=="},
                            {.type = AIMEE_BLK_THINKING,
                             .thinking_encrypted_content = "opaque-value",
                             .thinking_id = "opaque-id"}};
   append_message("current-turn", "webuser:owner", media, 3);
   poll(1);
   events = field(owned_observation, "events");
   last = field(field(cJSON_GetArrayItem(events, 0), "payload"), "payload");
   assert(cJSON_Compare(field(captured, "output"), last, 1));
   const char *display = string(captured, "result");
   assert(!display || !display[0]);
   char *wire = cJSON_PrintUnformatted(captured);
   assert(wire && strstr(wire, "opaque-value") && !strstr(wire, "POISONED"));
   free(wire);

   /* A completed older attempt is not an answer for the current attempt. */
   reset("running");
   job_status = "running";
   append_text("previous-attempt", "webuser:owner", "STALE PREVIOUS ANSWER");
   poll(1);
   assert(cJSON_GetArraySize(field(captured, "outputs")) == 0);
   absent("output");
   absent("result");
   assert(cJSON_GetArraySize(field(captured, "history")) == 1);

   reset("failed");
   job_status = "failed";
   append_text("current-turn", "webuser:owner", "partial observation");
   poll(0);
   absent("result");
   assert(field(captured, "output"));
   assert(!strcmp(string(captured, "diagnostic"), legacy));
   reset("failed");
   observation_rc = 1;
   job_status = "failed";
   poll(1);
   absent("result");
   absent("legacy_result");
   assert(!strcmp(string(captured, "diagnostic"), legacy));
   reset("completed");
   observation_rc = 1;
   poll(0);
   absent("result");
   absent("legacy_result");
   poll(1);
   absent("result");
   assert(!strcmp(string(captured, "legacy_result"), legacy));

   /* The owner/query boundary refuses metadata and diagnostics as well as text. */
   reset("completed");
   observation_rc = -2;
   job_status = "failed";
   poll(1);
   absent("role");
   absent("agent_name");
   absent("cursor_turn");
   absent("result");
   absent("output");
   absent("diagnostic");
   absent("legacy_result");
   reset("completed");
   observation_rc = -1;
   job_status = "failed";
   require_unavailable();
   reset("completed");
   cJSON_ReplaceItemInObject((cJSON *)field(owned_observation, "parent_source"), "actor",
                             cJSON_CreateString("webuser:other"));
   require_unavailable();
   reset("completed");
   append_text("current-turn", "webuser:owner", "valid prefix must not leak on later failure");
   append_text("current-turn", "webuser:other", "forged answer");
   require_unavailable();
   reset("completed");
   append_text("current-turn", "webuser:owner", "bad message");
   cJSON *bad = (cJSON *)field(
       field(cJSON_GetArrayItem(field(owned_observation, "events"), 0), "payload"), "payload");
   cJSON_ReplaceItemInObject(bad, "blocks", cJSON_CreateString("not an array"));
   require_unavailable();
   reset("completed");
   malformed_wire = "{\"schema\": \"unterminated";
   require_unavailable();
   reset("completed");
   cJSON_ReplaceItemInObject(owned_observation, "job_id", cJSON_CreateString("99"));
   require_unavailable();
   reset("completed");
   cJSON_AddStringToObject(owned_observation, "schema", "duplicate schema");
   require_unavailable();
   test_jobs_aux();
   cJSON_Delete(owned_observation);
   cJSON_Delete(captured);
   puts("delegate status canonical observation tests passed");
   return 0;
}

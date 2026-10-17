#include "server_compute_impl.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *log_entries;
static int writes, fail_store, disconnected, cancelled;
static char owner[64] = "alice";
int presence_turn_host_own(const char *sid, const char *turn)
{
   (void)sid;
   (void)turn;
   return 1;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
int db1_server_session_get(const char *id, db1_server_session_t *row)
{
   assert(!strcmp(id, "session-one"));
   memset(row, 0, sizeof(*row));
   snprintf(row->principal, sizeof(row->principal), "%s", owner);
   return 0;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   (void)id;
   (void)client;
   (void)principal;
   assert(0);
   return -1;
}
int db1_session_ir_append(const char *sid, const char *eid, const char *turn, const char *actor,
                          const char *payload, char sequence[32])
{
   assert(!strcmp(sid, "session-one") && !strcmp(actor, owner));
   assert(eid[0] && turn[0]);
   if (fail_store)
      return -1;
   cJSON *event = cJSON_Parse(payload);
   assert(event);
   cJSON_AddItemToArray(log_entries, event);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(log_entries));
   return 0;
}
int write_all(int fd, const char *text, size_t length)
{
   (void)fd;
   if (disconnected)
      return -1;
   if (length == 1 && text[0] == '\n')
      return 0;
   cJSON *envelope = cJSON_ParseWithLength(text, length);
   assert(envelope);
   int committed = cJSON_GetArraySize(log_entries);
   assert(committed > 0);
   char sequence[32];
   snprintf(sequence, sizeof(sequence), "%d", committed);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(envelope, "sequence")), sequence));
   assert(cJSON_Compare(cJSON_GetObjectItem(envelope, "event"),
                        cJSON_GetArrayItem(log_entries, committed - 1), 1));
   cJSON_Delete(envelope);
   writes++;
   return 0;
}
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static compute_ctx_t context(void)
{
   compute_ctx_t c = {0};
   c.presence_locked = 1;
   c.conn_alive = 1;
   c.write_mutex = &mutex;
   snprintf(c.vault_principal, sizeof(c.vault_principal), "alice");
   return c;
}
static void cleanup(compute_ctx_t *c)
{
   aimee_ir_session_set_sink(c->ir_previous_sink, c->ir_previous_context);
   cJSON_Delete(c->ir_tool_ids);
   cJSON_Delete(log_entries);
   log_entries = cJSON_CreateArray();
   writes = fail_store = disconnected = cancelled = 0;
}
static void response(void)
{
   aimee_block_t use = {.type = AIMEE_BLK_TOOL_USE, .tool_id = "opaque:one", .tool_name = "Bash"};
   aimee_message_t m = {.role = "assistant", .blocks = &use, .n_blocks = 1};
   aimee_session_event_t e = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "message-one", .message = &m};
   assert(!aimee_ir_session_emit(&e));
   assert(!aimee_ir_session_emit_tool_result("opaque:one", "done", 0));
   const cJSON *u = cJSON_GetArrayItem(
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload"),
                           "blocks"),
       0);
   const cJSON *r = cJSON_GetArrayItem(
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 3), "payload"),
                           "blocks"),
       0);
   const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(u, "tool_id"));
   assert(strstr(uid, "opaque:one") && strcmp(uid, "opaque:one"));
   assert(!strcmp(uid, cJSON_GetStringValue(cJSON_GetObjectItem(r, "tool_id"))));
}
int main(void)
{
   log_entries = cJSON_CreateArray();
   compute_ctx_t c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   assert(writes == 2);
   disconnected = 1;
   response();
   assert(!server_chat_ir_finish(&c, NULL));
   assert(!c.conn_alive && writes == 2 && cJSON_GetArraySize(log_entries) == 5);
   const cJSON *last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 4), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "completed"));
   assert(!server_chat_ir_finish(&c, NULL) && cJSON_GetArraySize(log_entries) == 5);
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   fail_store = 1;
   aimee_response_t ir = {.role = "assistant"};
   assert(aimee_ir_session_emit_response(&ir, "failed") < 0);
   assert(c.ir_failed && writes == 2 && cJSON_GetArraySize(log_entries) == 2);
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   cancelled = 1;
   assert(!server_chat_ir_finish(&c, NULL));
   last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "cancelled"));
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   assert(!server_chat_ir_finish(&c, NULL));
   last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "failed"));
   cleanup(&c);

   c = context();
   snprintf(owner, sizeof(owner), "bob");
   assert(server_chat_ir_begin(&c, "session-one", "hello") < 0 && !cJSON_GetArraySize(log_entries));
   snprintf(owner, sizeof(owner), "alice");
   c.presence_locked = 0;
   assert(server_chat_ir_begin(&c, "session-one", "hello") < 0);
   cJSON_Delete(log_entries);
   puts("server canonical capture tests passed");
   return 0;
}

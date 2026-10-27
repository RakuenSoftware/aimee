/* Real async admission/registry/IR owner; only scheduling, request TLS and model
 * execution are seams. This test performs no model request. */
#define main chat_ir_fixture_main
#include "test_server_chat_ir.c"
#undef main
#include "turn_registry.h"
#include "presence.h"
#include "modules/workspace/workspace_turn.h"
static inline int no_steering(const char *sid, cJSON **out)
{
   (void)sid;
   *out = NULL;
   return 0;
}
#define chat_steer_take no_steering
#include "../server/server_compute_async.c"
#undef chat_steer_take
static int worker_called;
int presence_session_owner(const char *sid, char *out, size_t cap)
{
   assert(!strcmp(sid, "session-one"));
   snprintf(out, cap, "alice");
   return 1;
}
presence_turn_result_t presence_turn_acquire_host(const char *sid, const char *principal,
                                                  char *turn, size_t cap, char *busy, size_t n)
{
   (void)busy;
   (void)n;
   assert(!strcmp(sid, "session-one") && !strcmp(principal, "alice"));
   snprintf(turn, cap, "presence-transport-only");
   return PRESENCE_TURN_ACQUIRED;
}
int presence_turn_release(const char *sid, const char *turn)
{
   assert(!strcmp(sid, "session-one") && !strcmp(turn, "presence-transport-only"));
   return 0;
}
void presence_emit_turn_started(const char *sid, const char *turn)
{
   (void)sid;
   (void)turn;
   assert(0);
}
void presence_emit_turn_done(const char *sid, const char *turn)
{
   (void)sid;
   (void)turn;
   assert(0);
}
void agent_set_request_cancel(atomic_int *flag)
{
   (void)flag;
}
void agent_set_request_session(const char *sid)
{
   assert(!strcmp(sid, "session-one"));
}
void agent_set_request_codex_creds(const char *a, const char *b)
{
   assert(!a && !b);
}
void compute_ctx_bind_request_context(const compute_ctx_t *c)
{
   (void)c;
}
void compute_ctx_clear_request_context(void)
{
}
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *p)
{
   (void)p;
   return NULL;
}
int server_session_pool_submit(server_ctx_t *server, const char *sid, void (*fn)(void *), void *arg,
                               int *threads)
{
   (void)server;
   (void)threads;
   assert(!strcmp(sid, "session-one"));
   fn(arg);
   return 0;
}
void chat_stream_worker(void *arg)
{
   compute_ctx_t *c = arg;
   worker_called++;
   char canonical[33];
   strcpy(canonical, c->ir_turn);
   assert(strlen(canonical) == 32 && strcmp(canonical, c->presence_turn_id));
   assert(turn_registry_match_exact("session-one", canonical) == 1);
   assert(turn_registry_match_exact("session-one", c->presence_turn_id) == -1);
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "current canonical turn"};
   aimee_message_t input = {.role = "user", .blocks = &block, .n_blocks = 1};
   assert(!server_chat_ir_begin(c, "session-one", &input));
   assert(!strcmp(canonical, c->ir_turn));
   char sequence[32];
   assert(!server_session_capture_cancel_owned("alice", "session-one", canonical, sequence,
                                               externally_cancel));
   assert(turn_registry_cancel_exact("session-one", canonical) == 1);
   int before = cJSON_GetArraySize(log_entries);
   assert(!server_chat_ir_finish(c, NULL));
   assert(cJSON_GetArraySize(log_entries) == before);
   server_chat_ir_close(c);
}
int main(void)
{
   log_entries = cJSON_CreateArray();
   turn_registry_init();
   compute_ctx_t c = context();
   c.presence_locked = 0;
   c.ir_delivery = 0;
   c.async_slot = -1;
   c.req = cJSON_Parse("{\"session_id\":\"session-one\"}");
   assert(c.req);
   assert(!chat_stream_dispatch(NULL, &c));
   assert(worker_called == 1 && !turn_registry_find("session-one"));
   cJSON_Delete(c.req);
   cJSON_Delete(log_entries);
   puts("canonical async admission/registry/capture cancellation passed");
   return 0;
}

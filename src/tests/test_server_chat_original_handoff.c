#define AIMEE_CHAT_ORIGINAL_TEST_MAIN chat_original_fixture_main
#define write_all                     handoff_fixture_write_all
#include "test_server_chat_original_input.c"
#undef write_all
#undef AIMEE_CHAT_ORIGINAL_TEST_MAIN
#include "modules/workspace/workspace_turn.h"
static int selected_case, finished, destroyed, consume_calls;
/* CMake supplies the ordinary config stub; minimal Make needs this fallback. */
__attribute__((weak)) _Thread_local int g_aimee_compute_threads_override;
void agent_set_durable_job(int job)
{
   (void)job;
}
void server_compute_budget_release(server_ctx_t *ctx, int grant)
{
   (void)ctx;
   (void)grant;
}
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *lease)
{
   (void)lease;
   return NULL;
}
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *lease)
{
   assert(!lease);
}
static host_text_status capture(const char *headers, const cJSON *request, void **owner_state,
                                void *state)
{
   (void)headers;
   (void)request;
   (void)state;
   *owner_state = &cookie;
   return HOST_TEXT_OK;
}
static const char *handoff_input_text(const cJSON *request)
{
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(request, "message");
   if (cJSON_IsString(input))
      return input->valuestring;
   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(input, "blocks");
   assert(cJSON_IsObject(input) && cJSON_GetArraySize(blocks) == 1);
   return cJSON_GetStringValue(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks, 0), "text"));
}
static host_text_status consume(void *owner_state, const cJSON *request,
                                host_original_submission_result *out, void *state)
{
   (void)state;
   assert(owner_state == &cookie);
   consume_calls++;
   assert(!strcmp(handoff_input_text(request), "raw synthetic input"));
   if (selected_case == 2)
      return HOST_TEXT_REFUSED;
   out->text = strdup("safe input");
   out->text_len = 10;
   out->text_capacity = 11;
   out->attachment = attachment();
   minted = out->attachment;
   if (selected_case == 4)
      assert(!pthread_cancel(pthread_self()));
   return HOST_TEXT_OK;
}
static void finish(void *owner_state, int admitted, void *state)
{
   (void)state;
   assert(owner_state == &cookie);
   finished++;
   assert(admitted == (selected_case == 1 || selected_case == 7));
}
static void destroy(void *owner_state, void *state)
{
   (void)state;
   assert(owner_state == &cookie);
   destroyed++;
}
static void cleanup_worker(void *slot)
{
   compute_ctx_free(*(compute_ctx_t **)slot);
   compute_ctx_lifetime_bind(NULL);
}
static void *handoff_worker(void *arg)
{
   compute_ctx_t *live = arg;
   compute_ctx_lifetime_bind(&live);
   pthread_cleanup_push(cleanup_worker, &live);
   int rc = server_chat_original_submission_prepare(live);
   if (!rc)
   {
      model_runs++;
      assert(live->original_admitted);
      assert(!strcmp(handoff_input_text(live->req), "safe input"));
   }
   else
      assert(!live->original_admitted);
   compute_ctx_free(live);
   assert(!live);
   pthread_cleanup_pop(0);
   compute_ctx_lifetime_bind(NULL);
   return NULL;
}
static void handoff_case(int which)
{
   selected_case = which;
   mode = which == 3 ? 6 : which == 5 ? 7 : 1;
   log_entries = cJSON_CreateArray();
   host_original_submission_owner ops = {capture, consume, finish, destroy};
   assert(!host_original_submission_install(&ops, NULL));
   assert(!server_input_attachment_install_owner(admit, created, NULL));
   const char *frame = "{\"message\":\"raw synthetic "
                       "input\",\"aimee_session_id\":\"session-one\",\"ir_version\":0}";
   if (which == 7)
      frame = "{\"message\":{\"role\":\"user\",\"blocks\":[{\"type\":\"text\","
              "\"text\":\"raw synthetic "
              "input\"}]},\"aimee_session_id\":\"session-one\",\"ir_version\":0}";
   host_original_submission *cap = NULL;
   assert(host_original_submission_capture("header", frame, strlen(frame), &cap) == HOST_TEXT_OK &&
          cap);
   host_original_submission **prior = host_original_submission_pending_bind(&cap);
   host_original_submission *transferred = host_original_submission_pending_take();
   assert(transferred && !cap && !host_original_submission_pending_take());
   host_original_submission_pending_bind(prior);
   if (which == 6)
   {
      host_original_submission_destroy(&transferred);
      assert(destroyed == 1 && !finished && !consume_calls);
      cJSON_Delete(log_entries);
      return;
   }
   compute_ctx_t *c = malloc(sizeof *c);
   *c = context();
   c->write_mutex = malloc(sizeof *c->write_mutex);
   assert(!pthread_mutex_init(c->write_mutex, NULL));
   c->conn_fd = -1;
   c->original_submission = transferred;
   c->req = cJSON_Parse(frame);
   pthread_t t;
   void *result;
   assert(!pthread_create(&t, NULL, handoff_worker, c) && !pthread_join(t, &result));
   assert(destroyed == 1 && finished == 1 && consume_calls == 1);
   if (which == 4 || which == 5)
      assert(result == PTHREAD_CANCELED && !model_runs);
   else
      assert(result == NULL && model_runs == (which == 1 || which == 7));
   assert(cJSON_GetArraySize(log_entries) == (which == 2 || which == 4     ? 0
                                              : (which == 1 || which == 7) ? 3
                                                                           : 2));
   cJSON_Delete(log_entries);
}
#ifndef AIMEE_CHAT_HANDOFF_TEST_MAIN
#define AIMEE_CHAT_HANDOFF_TEST_MAIN main
#endif
int AIMEE_CHAT_HANDOFF_TEST_MAIN(void)
{
   for (int i = 1; i <= 7; i++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         handoff_case(i);
         exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p);
      fprintf(stderr, "case %d status %d\n", i, status);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("original handoff: transfer, safe replacement, atomic admission, refusals, "
        "callback/consume cancellation and destruction passed");
   return 0;
}

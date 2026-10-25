/* Compose the real HTTP/retry/fence/executor with the established external
 * module peers. The imported fixture transports are renamed and unused. */
#define main                           context_fixture_unused_main
#define http_retry_post_observed_bytes fixture_http_retry_post_observed_bytes
#define http_retry_post_context_bytes  fixture_http_retry_post_context_bytes
#define http_retry_post_context        fixture_http_retry_post_context
#define http_retry_post                fixture_http_retry_post
#include "test_agent_context_refusal.c"
#undef main
#undef http_retry_post_observed_bytes
#undef http_retry_post_context_bytes
#undef http_retry_post_context
#undef http_retry_post
#include "wire_fence.h"
#include "agent_dispatch_scope.h"
#include "agent_config.h"
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent_admission.h"
#include "provider_cli_adapter.h"

#include "agent_tasks.h"
#include "db1_client/agent_jobs.h"
#include <limits.h>
static int job_store_count, job_store_failure, job_store_writes;
static char job_store_tool[64];
static int job_refuse_at_model, job_model_phases;
static int observer_probe;
static void progress_observer_probe(void *context, int status)
{
   assert(context == &observer_probe && status == 200);
   observer_probe++;
}
int fixture_job_get(int id, db1_agent_job_t *out)
{
   *out = (db1_agent_job_t){.id = id, .api_call_count = job_store_count};
   return job_store_failure ? -1 : 0;
}
int fixture_job_is_cancelled(int id)
{
   assert(id == 91);
   return 0;
}
void fixture_job_update(int id, const char *status, int cursor, const char *result)
{
   assert(id == 91 && status && cursor >= 0);
   (void)result;
}
void fixture_job_free(db1_agent_job_t *out)
{
   (void)out;
}
void fixture_job_heartbeat_ext(int id, const char *tool, int count)
{
   assert(id == 91 && tool && count >= job_store_count);
   job_store_count = count;
   snprintf(job_store_tool, sizeof job_store_tool, "%s", tool);
   if (!strcmp(tool, "model") && ++job_model_phases == job_refuse_at_model)
   {
      request_context_t refused = {.context_refused = 1};
      request_context_set(&refused);
   }
   job_store_writes++;
}
static void *progress_callback(void *held)
{
   agent_job_progress_binding_t previous = agent_job_progress_bind(held);
   assert(agent_get_durable_job_id() == 91);
   for (int i = 0; i < 20; i++)
      agent_job_progress_observe_transport(1);
   agent_job_progress_restore(previous);
   assert(!agent_get_durable_job_id());
   return NULL;
}
static void test_progress_lifetime(void)
{
   wire_fence_transport_observer_t outer_observer = wire_fence_transport_observer_bind(
       (wire_fence_transport_observer_t){progress_observer_probe, &observer_probe});
   job_store_count = 13;
   assert(!agent_durable_job_begin(91));
   agent_job_progress_t *held = agent_job_progress_capture();
   agent_job_progress_binding_t nested = agent_job_progress_bind(held);
   assert(!agent_durable_job_begin(92));
   agent_set_durable_job(0);
   agent_job_progress_restore(nested);
   assert(agent_get_durable_job_id() == 91);
   wire_fence_transport_observer_t actual =
       wire_fence_transport_observer_bind((wire_fence_transport_observer_t){0});
   actual.completed(actual.context, 200);
   (void)wire_fence_transport_observer_bind(actual);
   assert(job_store_count == 14); /* one notification, even same-owner rebind */
   agent_set_durable_job(0);
   actual = wire_fence_transport_observer_bind((wire_fence_transport_observer_t){0});
   assert(actual.completed == progress_observer_probe && actual.context == &observer_probe);
   actual.completed(actual.context, 200);
   assert(observer_probe == 1);
   (void)wire_fence_transport_observer_bind(actual);
   assert(!agent_durable_job_begin(91));

   agent_set_durable_job(0);
   pthread_t a, b;
   assert(!pthread_create(&a, NULL, progress_callback, held));
   assert(!pthread_create(&b, NULL, progress_callback, held));
   assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
   agent_job_progress_release(held);
   assert(job_store_count == 54);
   assert(!agent_durable_job_begin(91)); /* actual persisted baseline */
   assert(agent_job_progress_transport_attempts() == 54);
   int writes = job_store_writes;
   agent_job_progress_observe_transport(0);
   assert(job_store_writes == writes);
   job_store_failure = 1;
   assert(agent_durable_job_begin(92) == -1 && agent_get_durable_job_id() == 91);
   job_store_failure = 0;
   agent_set_durable_job(0);
   job_store_count = INT_MAX - 1;
   assert(!agent_durable_job_begin(91));
   agent_job_progress_observe_transport(3);
   agent_job_progress_observe_transport(1);
   assert(job_store_count == INT_MAX);
   agent_set_durable_job(0);
   (void)wire_fence_transport_observer_bind(outer_observer);
}

/* This peer refuses execution. It observes the real guarded dispatcher's live
 * admission slot; it never affirms a source/host/model or starts a child. */
static int admission_probe_active, admission_probe_calls;
static const char *admission_probe_model;
int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   assert(admission_probe_active && cfg && cfg->agent);
   assert(agent_admission_agent_active(cfg->agent->name) == 1);
   assert(!strcmp(cfg->agent->model, admission_probe_model));
   assert(!strcmp(out->model, admission_probe_model) &&
          !strcmp(out->served_model, admission_probe_model));
   admission_probe_calls++;
   snprintf(out->error, sizeof out->error, "fixture refuses native execution after admission");
   return AGENT_RC_CONTEXT_REFUSED;
}
static int guarded_admission_probe(agent_t *agent)
{
   agent_network_t network = {0};
   agent_result_t result = {0};
   admission_probe_active = 1;
   admission_probe_model = agent->model;
   request_context_clear();
   int rc =
       fixture_agent_dispatch_text(agent, &network, NULL, "system", "task", 128, 0, 1, &result);
   admission_probe_active = 0;
   assert(!result.success && !result.canonical_response.n_content);
   assert(!agent->model[0] || !strcmp(result.model, agent->model));
   agent_result_clear_response(&result);
   return rc;
}
static void test_actual_acp_admission_unknown_model_bucket(void)
{
   agent_t agent = {0};
   strcpy(agent.name, "acp-admission-a");
   strcpy(agent.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(agent.cli_kind, "acp");
   agent.max_parallel = 1;
   agent_dispatch_set_fail_fast(1);
   admission_probe_calls = 0;
   assert(guarded_admission_probe(&agent) == AGENT_RC_CONTEXT_REFUSED);
   assert(admission_probe_calls == 1 && !agent.model[0]);
   assert(agent_admission_global_active() == 0);

   agent_admission_configure(16, 1, NULL, 0);
   agent_admit_req_t held = {.ctx_handle = "fixture-held",
                             .agent = "other-acp-alias",
                             .model = "acp:unknown-model",
                             .per_agent_max = 1,
                             .flags = AGENT_ADMIT_NONBLOCKING};
   agent_slot_t *slot = agent_admission_acquire(&held, NULL);
   assert(slot);
   strcpy(agent.name, "acp-admission-b");
   assert(guarded_admission_probe(&agent) == AGENT_RC_AT_LIMIT);
   assert(admission_probe_calls == 1); /* Distinct aliases share the unknown model cap. */
   strcpy(agent.model, "declared-model");
   assert(guarded_admission_probe(&agent) == AGENT_RC_CONTEXT_REFUSED);
   assert(admission_probe_calls == 2); /* A real model keeps its own bucket/metadata. */
   agent_admission_release(slot);

   held.agent = agent.name;
   held.model = "different-model";
   slot = agent_admission_acquire(&held, NULL);
   assert(slot);
   agent.model[0] = 0;
   assert(guarded_admission_probe(&agent) == AGENT_RC_AT_LIMIT);
   assert(admission_probe_calls == 2); /* Per-agent cap is still enforced. */
   agent_admission_release(slot);
   assert(guarded_admission_probe(&agent) == AGENT_RC_CONTEXT_REFUSED);
   assert(admission_probe_calls == 3 && !agent.model[0]);

   strcpy(agent.cli_kind, "claude");
   assert(guarded_admission_probe(&agent) == AGENT_RC_AT_LIMIT);
   assert(admission_probe_calls == 3); /* No generic model-less bypass. */
   assert(agent_admission_global_active() == 0);
   agent_dispatch_set_fail_fast(0);
   puts("ACP actual dispatcher: unknown admission bucket bounded; metadata stays unknown; no "
        "execution");
}

/* No operator role definition and an empty permission response from the
 * external role-owner peer: these cases exercise text-only routing, not grants. */
char *role_template_frontmatter(const char *root, const char *role)
{
   (void)root;
   (void)role;
   return NULL;
}
static int retry_role_permissions(const uint8_t *request, size_t length, uint8_t *response,
                                  size_t capacity, size_t *used)
{
   (void)request;
   (void)length;
   aimee_delegates_wire_t w;
   aimee_delegates_wire_init(&w, response, capacity);
   aimee_delegates_wire_u32(&w, AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC);
   aimee_delegates_wire_u32(&w, 0); /* grants */
   aimee_delegates_wire_u32(&w, 0); /* unenforced */
   aimee_delegates_wire_u32(&w, 0); /* denied tools */
   *used = w.len;
   return w.overflow ? -1 : 0;
}

static int listening_peer(char url[128])
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(!bind(fd, (struct sockaddr *)&address, sizeof address));
   assert(!listen(fd, 4));
   socklen_t length = sizeof address;
   assert(!getsockname(fd, (struct sockaddr *)&address, &length));
   snprintf(url, 128, "http://127.0.0.1:%u/v1/chat/completions", ntohs(address.sin_port));
   return fd;
}
static void receive_post(int fd)
{
   char request[65536] = {0};
   size_t used = 0, content_length = 0;
   char *headers = NULL;
   do
   {
      assert(used + 1 < sizeof request);
      ssize_t n = recv(fd, request + used, sizeof request - used - 1, 0);
      assert(n > 0);
      used += (size_t)n;
      request[used] = 0;
      headers = strstr(request, "\r\n\r\n");
      if (headers)
      {
         const char *length = strcasestr(request, "Content-Length:");
         assert(length && length < headers);
         content_length = (size_t)strtoul(length + strlen("Content-Length:"), NULL, 10);
      }
   } while (!headers || used < (size_t)(headers + 4 - request) + content_length);
   assert(!strncmp(request, "POST ", 5));
}
static pid_t serve_peer(int listener, const int *statuses, const char *const *bodies, int count)
{
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      alarm(15);
      for (int i = 0; i < count; i++)
      {
         int fd = accept(listener, NULL, NULL);
         assert(fd >= 0);
         receive_post(fd);
         char response[4096];
         int length =
             snprintf(response, sizeof response,
                      "HTTP/1.1 %d Test\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                      statuses[i], strlen(bodies[i]), bodies[i]);
         assert(length > 0 && (size_t)length < sizeof response);
         assert(send(fd, response, (size_t)length, 0) == length);
         close(fd);
      }
      close(listener);
      _exit(0);
   }
   return child;
}
static void join_peer(pid_t child, int listener)
{
   int status = 0;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   close(listener);
}
static const char known_zero[] =
    "{\"model\":\"gpt-4o\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":"
    "\"assistant\",\"content\":\"Measured "
    "zero.\"}}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0}}";
static const char unknown_usage[] =
    "{\"model\":\"gpt-4o\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":"
    "\"assistant\",\"content\":\"Unknown usage.\"}}]}";
#include "agent_result_cache.h"
#include <aimee/translation/aimee_backend.h>
static char *cached_response;
char *fixture_cache_get(const char *role, const char *key)
{
   assert(!strcmp(role, "summarize") && key && *key);
   return strdup(cached_response);
}
void fixture_cache_put(const char *role, const char *key, const char *value)
{
   (void)role;
   (void)key;
   (void)value;
   abort();
}
static void test_cached_result_no_transport(void)
{
   aimee_response_t response = {0};
   char error[128];
   assert(!aimee_backend_parse_wire(known_zero, strlen(known_zero), AIMEE_WIRE_OPENAI_CHAT,
                                    &response, error, sizeof error));
   cJSON *json = aimee_ir_response_json(&response);
   cached_response = cJSON_PrintUnformatted(json);
   assert(cached_response);
   cJSON_Delete(json);
   aimee_response_free(&response);
   job_store_count = 11;
   assert(!agent_durable_job_begin(91));
   int writes = job_store_writes;
   agent_result_t result = {0};
   assert(agent_result_cache_read("summarize", "fixture-key", &result));
   assert(result.cache_hit && result.success && job_store_count == 11 &&
          job_store_writes == writes && agent_job_progress_transport_attempts() == 11);
   agent_result_clear_response(&result);
   free(cached_response);
   cached_response = NULL;
   agent_set_durable_job(0);
}
static int progress_stream(const char *bytes, size_t length, void *context)
{
   (void)bytes;
   *(size_t *)context += length;
   return 0;
}
static void test_actual_stream_progress(void)
{
   char url[128];
   int listener = listening_peer(url), statuses[] = {200};
   const char *bodies[] = {known_zero};
   pid_t child = serve_peer(listener, statuses, bodies, 1);
   job_store_count = 5;
   assert(!agent_durable_job_begin(91));
   size_t bytes = 0;
   assert(wire_fence_post_stream(url, "", "{}", 2, progress_stream, &bytes, 1000, "", "openai",
                                 "gpt-4o", WIRE_FENCE_OPENAI_CHAT) == 200);
   assert(bytes && job_store_count == 6);
   join_peer(child, listener);
   request_context_t refused = {.context_refused = 1};
   request_context_set(&refused);
   assert(wire_fence_post_stream("http://127.0.0.1:1", "", "{}", 2, progress_stream, &bytes, 1000,
                                 "", "openai", "gpt-4o",
                                 WIRE_FENCE_OPENAI_CHAT) == HTTP_RETRY_ADMISSION_REFUSED);
   request_context_clear();
   assert(job_store_count == 6);
   agent_set_durable_job(0);
}
static void test_plain_refusal_clears_phase(void)
{
   for (int fallback = 0; fallback < 2; fallback++)
   {
      char url[128];
      int listener = listening_peer(url);
      pid_t child = 0;
      int statuses[] = {400};
      const char *bodies[] = {"{}"};
      if (fallback)
         child = serve_peer(listener, statuses, bodies, 1);
      agent_t agent = {0};
      strcpy(agent.name, "phase-clear");
      strcpy(agent.model, "gpt-4o");
      strcpy(agent.provider, "openai");
      strcpy(agent.auth_type, "none");
      strcpy(agent.endpoint, url);
      if (fallback)
         strcpy(agent.fallback_model, "fallback-model");
      agent.timeout_ms = 1000;
      request_context_snapshot_t previous;
      request_context_snapshot(&previous);
      job_store_count = 3;
      assert(!agent_durable_job_begin(91));
      job_refuse_at_model = fallback + 1;
      job_model_phases = 0;
      agent_result_t result = {0};
      assert(fixture_agent_execute_text(&agent, "system", "task", 128, 0, &result) ==
             AGENT_RC_CONTEXT_REFUSED);
      assert(job_model_phases == fallback + 1 && job_store_count == 3 + fallback &&
             !job_store_tool[0]);
      job_refuse_at_model = 0;
      request_context_restore(&previous);
      agent_set_durable_job(0);
      agent_result_clear_response(&result);
      struct pollfd pending = {.fd = listener, .events = POLLIN};
      assert(poll(&pending, 1, 0) == 0);
      if (fallback)
         join_peer(child, listener);
      else
         close(listener);
   }
}
static void test_actual_executor_observations(int only_mode)
{
   const int statuses[][2] = {{200, 0}, {400, 0},   {204, 0}, {500, 200},
                              {200, 0}, {400, 200}, {200, 0}};
   const char *bodies[][2] = {{"not-json", NULL},   {"{}", NULL},       {"", NULL},
                              {"{}", known_zero},   {known_zero, NULL}, {"{}", known_zero},
                              {unknown_usage, NULL}};
   job_store_count = 7;
   assert(!agent_durable_job_begin(91));
   int expected_attempts = 7;
   for (int execution = 1; execution >= 0; execution--)
      for (int mode = 0; mode < 7; mode++)
      {
         if (only_mode >= 0 && mode != only_mode)
            continue;
         char url[128];
         int listener = listening_peer(url), count = mode == 3 || mode == 5 ? 2 : 1;
         pid_t child = serve_peer(listener, statuses[mode], bodies[mode], count);
         agent_t agent = {0};
         strcpy(agent.name, "transport-usage");
         strcpy(agent.model, "gpt-4o");
         strcpy(agent.provider, "openai");
         strcpy(agent.auth_type, "none");
         strcpy(agent.endpoint, url);
         if (mode == 5)
            strcpy(agent.fallback_model, "fallback-model");
         agent.max_turns = 2;
         agent.timeout_ms = 10000;
         agent_network_t network = {0};
         agent_result_t result = {0};
         int rc = execution ? fixture_agent_execute_text(&agent, "system", "task", 128, 0, &result)
                            : fixture_agent_role_text(&agent, &network, NULL, "system", "task", 128,
                                                      0, &result);
         expected_attempts += count;
         assert(job_store_count == expected_attempts);
         assert(agent_job_progress_transport_attempts() == expected_attempts);
         assert((rc == 0 && result.success) == (mode >= 3));
         assert(agent_result_usage_is_unobserved(&result) == (mode != 4));
         if (mode >= 3)
         {
            assert(result.canonical_response.usage_unobserved == (mode != 4));
            assert(!aimee_ir_response_admit(&result.canonical_response));
         }
         agent_result_clear_response(&result);
         join_peer(child, listener);
      }
   agent_set_durable_job(0);
   assert(!agent_durable_job_begin(91));
   assert(agent_job_progress_transport_attempts() == expected_attempts);
   agent_set_durable_job(0);
   puts("provider attempts: plain and tool HTTP decode/status/empty/retry/fallback unknown, first "
        "knownzero "
        "remains known");
}
static void test_actual_retry_observer_and_no_send(void)
{
   char url[128];
   int listener = listening_peer(url);
   int statuses[] = {500, 200};
   const char *bodies[] = {"{}", known_zero};
   pid_t child = serve_peer(listener, statuses, bodies, 2);
   wire_fence_observation_t observation = {.transport_attempts = 99};
   char *response = NULL;
   int status =
       wire_fence_post_observed(url, NULL, "{}", 2, &response, 1000, NULL, 2, 1, 1, "fixture",
                                "fixture", NULL, WIRE_FENCE_OPENAI_CHAT, &observation);
   assert(status == 200 && observation.transport_attempts == 2);
   free(response);
   join_peer(child, listener);
   for (int mode = 0; mode < 3; mode++)
   {
      listener = listening_peer(url);
      request_context_t context = {0};
      atomic_int cancelled = 0;
      if (mode == 0)
         context.context_refused = 1;
      else if (mode == 1)
      {
         strcpy(context.memory_source_release, "required-source");
         context.memory_receipt_required = 1; /* No send-guard owner installed: refuse. */
      }
      else
         atomic_store(&cancelled, 1);
      request_context_set(&context);
      atomic_int *previous_cancel = agent_get_request_cancel();
      agent_set_request_cancel(&cancelled);
      response = NULL;
      observation.transport_attempts = 99;
      status =
          wire_fence_post_observed(url, NULL, "{}", 2, &response, 1000, NULL, 2, 1, 1, "fixture",
                                   "fixture", NULL, WIRE_FENCE_OPENAI_CHAT, &observation);
      assert(status != 200 && observation.transport_attempts == 0 && !response);
      agent_set_request_cancel(previous_cancel);
      request_context_clear();
      struct pollfd pending = {.fd = listener, .events = POLLIN};
      if (poll(&pending, 1, 0) > 0)
      {
         int connection = accept(listener, NULL, NULL);
         assert(connection >= 0);
         char byte;
         assert(recv(connection, &byte, 1, 0) == 0); /* A guarded connection is not a model POST. */
         close(connection);
      }
      close(listener);
   }
   puts("provider attempts: actual retries counted; preflight/guard/cancel no-send stays zero");
}
/* Resource lifecycle callbacks are explicitly synthetic here. Actual guarded
 * dispatch, admission, routing and HTTP work remain production. This proves
 * ordering/selected-attempt coverage, not a real container or source grant. */
typedef struct
{
   int active, begins, ends, poisoned, fail_end, setup_delay_ms;
   char selected[2][MAX_AGENT_NAME];
} fixture_dispatch_lifetime_t;
static int fixture_dispatch_begin(const agent_t *selected, void *context, char *error,
                                  size_t capacity)
{
   fixture_dispatch_lifetime_t *state = context;
   assert(!state->active);
   if (state->poisoned)
   {
      snprintf(error, capacity, "fixture resource refused after uncertain retirement");
      return -1;
   }
   assert(state->begins < 2);
   snprintf(state->selected[state->begins], sizeof state->selected[0], "%s", selected->name);
   state->begins++;
   if (state->setup_delay_ms)
      usleep((useconds_t)state->setup_delay_ms * 1000);
   state->active = 1;
   return 0;
}
static int fixture_dispatch_end(void *context, char *error, size_t capacity)
{
   fixture_dispatch_lifetime_t *state = context;
   assert(state->active);
   state->active = 0;
   state->ends++;
   if (state->fail_end)
   {
      state->poisoned = 1;
      snprintf(error, capacity, "fixture resource retirement unconfirmed");
      return -1;
   }
   return 0;
}
static const char *fixture_active_delegation;
const char *delegation_active_id(void)
{
   return fixture_active_delegation;
}
static void test_dispatch_resource_refusal(void)
{
   agent_t agent = {0};
   strcpy(agent.name, "resource-lifetime");
   strcpy(agent.model, "gpt-4o");
   strcpy(agent.provider, "openai");
   strcpy(agent.auth_type, "none");
   agent.max_parallel = 1;
   agent.timeout_ms = 10000;
   char url[128];
   int listener = listening_peer(url);
   strcpy(agent.endpoint, url);
   agent_result_t result = {0};
   fixture_active_delegation = "owned-test-delegation";
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   fixture_active_delegation = NULL;
   assert(!result.success && !result.response);
   struct pollfd pending = {.fd = listener, .events = POLLIN};
   assert(poll(&pending, 1, 0) == 0);
   fixture_dispatch_lifetime_t state = {.fail_end = 1};
   agent_dispatch_scope_t scope = {
       .begin = fixture_dispatch_begin, .end = fixture_dispatch_end, .context = &state};
   const agent_dispatch_scope_t *previous = agent_dispatch_scope_bind(&scope);
   atomic_int cancelled = 1;
   atomic_int *previous_cancel = agent_get_request_cancel();
   agent_set_request_cancel(&cancelled);
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   agent_set_request_cancel(previous_cancel);
   assert(state.begins == 0 && state.ends == 0 && poll(&pending, 1, 0) == 0);
   request_context_snapshot_t previous_owner;
   request_context_snapshot(&previous_owner);
   request_context_t refused = {.context_refused = 1};
   request_context_set(&refused);
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   request_context_restore(&previous_owner);
   assert(state.begins == 0 && state.ends == 0 && poll(&pending, 1, 0) == 0);
   state.fail_end = 0;
   state.setup_delay_ms = 30;
   agent.tool_loop_timeout_ms_cap = 1;
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   assert(state.begins == 1 && state.ends == 1 && !state.active);
   assert(agent.tool_loop_timeout_ms_cap == 1); /* Runtime copy only; no caller mutation. */
   assert(poll(&pending, 1, 0) == 0);
   agent.tool_loop_timeout_ms_cap = 0;
   state = (fixture_dispatch_lifetime_t){.fail_end = 1};
   int status = 200;
   const char *body = known_zero;
   pid_t child = serve_peer(listener, &status, &body, 1);
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CAPTURE_FAILED);
   assert(!result.success && state.begins == 1 && state.ends == 1 && !state.active);
   agent_result_clear_response(&result);
   join_peer(child, listener);
   listener = listening_peer(url);
   strcpy(agent.endpoint, url);
   assert(fixture_agent_dispatch_text(&agent, NULL, NULL, NULL, "task", 128, 0, 0, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   assert(!result.success && state.begins == 1 && state.ends == 1);
   pending = (struct pollfd){.fd = listener, .events = POLLIN};
   assert(poll(&pending, 1, 0) == 0);
   close(listener);
   agent_dispatch_scope_bind(previous);
   assert(agent_dispatch_scope_current() == previous);
}

/* Exercise the real routing owner, configured fallback chain and same-tier
 * selection. Only the HTTP endpoints and existing external module peers are
 * fixtures; dispatcher admission/health/IR parsing remain production code. */
static void test_actual_routing_retry_usage(void)
{
   const char *first_bodies[] = {"{\"error\":{\"message\":\"invalid_api_key\"}}",
                                 "{\"error\":{\"message\":\"invalid_api_key\"},"
                                 "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}"};
   for (int route = 0; route < 3; route++)
      for (int known = 0; known < (route ? 1 : 2); known++)
      {
         agent_config_t *cfg = calloc(1, sizeof *cfg);
         assert(cfg);
         cfg->agent_count = 2;
         int listeners[2];
         pid_t children[2];
         for (int a = 0; a < 2; a++)
         {
            agent_t *agent = &cfg->agents[a];
            snprintf(agent->name, sizeof agent->name, "retry-%d-%d-%d", route, known, a);
            snprintf(agent->model, sizeof agent->model, "fixture-model-%d", a);
            strcpy(agent->provider, "openai");
            strcpy(agent->auth_type, "none");
            strcpy(agent->roles[0], "review");
            agent->role_count = 1;
            agent->enabled = 1;
            agent->max_parallel = 1;
            agent->max_turns = 2;
            agent->timeout_ms = 10000;
            char url[128];
            listeners[a] = listening_peer(url);
            strcpy(agent->endpoint, url);
            int status = a || known ? 200 : 401;
            const char *body = a ? known_zero : first_bodies[known];
            children[a] = serve_peer(listeners[a], &status, &body, 1);
         }
         strcpy(cfg->default_delegate, cfg->agents[0].name);
         if (route == 1)
         {
            cfg->fallback_count = 1;
            strcpy(cfg->fallback_chain[0], cfg->agents[1].name);
         }
         fixture_dispatch_lifetime_t resources = {0};
         agent_dispatch_scope_t scope = {
             .begin = fixture_dispatch_begin, .end = fixture_dispatch_end, .context = &resources};
         const agent_dispatch_scope_t *previous = agent_dispatch_scope_bind(&scope);
         agent_result_t result = {0};
         int rc = route == 0 ? agent_run_ex(cfg, "review", "system", "task", 128, 0, &result)
                             : agent_run_with_tools(cfg, "review", "system", "task", 128, &result);
         agent_dispatch_scope_bind(previous);
         assert(resources.begins == 2 && resources.ends == 2 && !resources.active);
         assert(!strcmp(resources.selected[0], cfg->agents[0].name));
         assert(!strcmp(resources.selected[1], cfg->agents[1].name));
         if (rc || !result.success)
            fprintf(stderr, "route %d known %d failed rc=%d: %s\n", route, known, rc, result.error);
         assert(!rc && result.success);
         assert(!strcmp(result.agent_name, cfg->agents[1].name));
         assert(agent_result_usage_is_unobserved(&result));
         assert(result.canonical_response.usage_unobserved);
         assert(!result.canonical_response.usage_in && !result.canonical_response.usage_out);
         assert(!aimee_ir_response_admit(&result.canonical_response));
         agent_result_clear_response(&result);
         for (int a = 0; a < 2; a++)
            join_peer(children[a], listeners[a]);
         free(cfg);
      }
   puts("provider retry owners: actual plain route/configured chain/same-tier retain unknown or "
        "prior paid usage");
}

static void test_retry_owner_known_usage_boundaries(void)
{
   for (int prior = 0; prior < 4; prior++)
   {
      agent_t agent = {0};
      snprintf(agent.name, sizeof agent.name, "retry-boundary-%d", prior);
      strcpy(agent.model, "gpt-4o");
      strcpy(agent.provider, "openai");
      strcpy(agent.auth_type, "none");
      agent.max_parallel = 1;
      agent.timeout_ms = 10000;
      char url[128];
      int listener = listening_peer(url), status = 200;
      strcpy(agent.endpoint, url);
      const char *body = known_zero;
      pid_t child = serve_peer(listener, &status, &body, 1);
      /* An initialized owned diagnostic result, not a fabricated source receipt.
       * Primary, cached and reasoning counters all prevent latest-model pricing. */
      agent_result_t out = {0};
      if (prior == 1)
         out.prompt_tokens = 7;
      if (prior == 2)
         out.canonical_response.usage_cache_read = 3;
      if (prior == 3)
         out.canonical_response.usage_reasoning = 2;
      assert(!fixture_agent_retry_text(&agent, NULL, NULL, "system", "task", 128, 0, 0, &out));
      assert(out.success && agent_result_usage_is_unobserved(&out) == (prior != 0));
      assert(out.canonical_response.usage_unobserved == (prior != 0));
      assert(!aimee_ir_response_admit(&out.canonical_response));
      agent_result_clear_response(&out);
      join_peer(child, listener);
   }
   puts("provider retry owner: observed zero preserved; prior primary/cache/reasoning work cannot "
        "price at final model");
}

int main(int argc, char **argv)
{
   int only_mode = -1;
   int progress_only = argc == 2 && !strcmp(argv[1], "progress-buffered");
   int lifecycle_only = argc == 2 && !strcmp(argv[1], "dispatch-lifecycle");
   if (argc == 2)
   {
      assert(progress_only || lifecycle_only || !strcmp(argv[1], "bad-json") ||
             !strcmp(argv[1], "retry-only"));
      only_mode = !strcmp(argv[1], "bad-json") ? 0 : 3;
   }
   else
      assert(argc == 1);
   delegate_role_seam_install();
   delegate_register_permissions_provider(retry_role_permissions);
   agent_tools_register_metadata(fixture_metadata);
   char home[4096];
   snprintf(home, sizeof home, "%s/provider-usage-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home));
   assert(!platform_setenv("AIMEE_HOME", home));
   assert(!platform_unsetenv("AIMEE_CONFIG_PATH"));
   assert(!platform_setenv("AIMEE_CONTEXT_NO_KB", "1"));
   assert(!config_set_memory_recall_enabled(0));
   recall_reply = "{\"status\":\"ok\",\"recall\":{}}";
   if (progress_only)
   {
      test_actual_executor_observations(4);
      return 0;
   }
   if (lifecycle_only)
   {
      test_actual_routing_retry_usage();
      return 0;
   }
   test_progress_lifetime();
   test_actual_stream_progress();
   test_cached_result_no_transport();
   test_plain_refusal_clears_phase();
   test_dispatch_resource_refusal();
   test_actual_routing_retry_usage();
   test_retry_owner_known_usage_boundaries();
   test_actual_executor_observations(only_mode);
   test_actual_retry_observer_and_no_send();
   test_actual_acp_admission_unknown_model_bucket();
   return 0;
}

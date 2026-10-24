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
#include "agent_config.h"
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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
static void test_actual_executor_observations(int only_mode)
{
   const int statuses[][2] = {{200, 0}, {400, 0},   {204, 0}, {500, 200},
                              {200, 0}, {400, 200}, {200, 0}};
   const char *bodies[][2] = {{"not-json", NULL},   {"{}", NULL},       {"", NULL},
                              {"{}", known_zero},   {known_zero, NULL}, {"{}", known_zero},
                              {unknown_usage, NULL}};
   for (int execution = 0; execution < 2; execution++)
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
         int rc = execution ? agent_execute(&agent, "system", "task", 128, 0, &result)
                            : agent_execute_with_tools_for_role(&agent, &network, NULL, "system",
                                                                "task", 128, 0, &result);
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
         agent_result_t result = {0};
         int rc = route == 0 ? agent_run_ex(cfg, "review", "system", "task", 128, 0, &result)
                             : agent_run_with_tools(cfg, "review", "system", "task", 128, &result);
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
      assert(!agent_retry_dispatch_one(&agent, NULL, NULL, "system", "task", 128, 0, 0, &out));
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
   if (argc == 2)
   {
      assert(!strcmp(argv[1], "bad-json") || !strcmp(argv[1], "retry-only"));
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
   test_actual_routing_retry_usage();
   test_retry_owner_known_usage_boundaries();
   test_actual_executor_observations(only_mode);
   test_actual_retry_observer_and_no_send();
   return 0;
}

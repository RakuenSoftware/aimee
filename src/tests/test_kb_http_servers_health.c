#include "kb/http/kb_http_servers.h"
#include "kb_reqctx.h"
#include "modules/db2/c/db2_tenant.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static kb_management_health_result_t next_result;
static unsigned int calls;
static int64_t got_team;
static char got_server[128];
static char got_subject[128];
static int64_t got_list_team;
static pthread_mutex_t block_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_cv = PTHREAD_COND_INITIALIZER;
static int callback_entered;
static int callback_release;
static unsigned action_calls;
static unsigned read_calls;
static char action_body[128];
static int scope_begins, scope_commits;
static int64_t scope_team;
static int next_scope_result;

int db2_tenant_scope_begin(const kb_principal_t *actor, int64_t team)
{
   assert(actor && actor->authenticated);
   scope_begins++;
   scope_team = team;
   return next_scope_result;
}
int db2_tenant_scope_commit(void)
{
   scope_commits++;
   return 0;
}
void db2_tenant_scope_rollback(void)
{
}

static kb_management_action_result_t action_handler_fn(void *ctx, const kb_principal_t *actor,
                                                       int64_t team, const char *server,
                                                       const char *body, size_t body_len)
{
   assert(ctx == (void *)0x4321 && actor && actor->authenticated && team == 9);
   assert(!strcmp(server, "server-a") && body_len < sizeof(action_body));
   memcpy(action_body, body, body_len);
   action_body[body_len] = 0;
   action_calls++;
   return KB_MANAGEMENT_ACTION_OK;
}

static kb_management_action_result_t blocking_action_handler(void *ctx, const kb_principal_t *actor,
                                                             int64_t team, const char *server,
                                                             const char *body, size_t body_len)
{
   (void)ctx;
   (void)actor;
   (void)team;
   (void)server;
   (void)body;
   (void)body_len;
   pthread_mutex_lock(&block_mu);
   callback_entered = 1;
   pthread_cond_broadcast(&block_cv);
   while (!callback_release)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   return KB_MANAGEMENT_ACTION_OK;
}

static kb_management_read_result_t read_result;
static kb_management_read_result_t read_handler_fn(void *ctx, const kb_principal_t *actor,
                                                   int64_t team, const char *server,
                                                   server_mgmt_read_selector_t selector, char *out,
                                                   size_t cap)
{
   assert(ctx == (void *)0x2468 && actor && actor->authenticated && team == 9);
   assert(!strcmp(server, "server-a"));
   assert(selector == SERVER_MGMT_READ_SELECTOR_AGENTS ||
          selector == SERVER_MGMT_READ_SELECTOR_CONFIG);
   read_calls++;
   if (read_result == KB_MANAGEMENT_READ_OK)
      snprintf(out, cap,
               selector == SERVER_MGMT_READ_SELECTOR_AGENTS
                   ? "{\"server_id\":\"server-a\",\"team\":9,\"agents\":[]}"
                   : "{\"server_id\":\"server-a\",\"team\":9,\"config\":{}}");
   return read_result;
}

static kb_management_read_result_t blocking_read_handler(void *ctx, const kb_principal_t *actor,
                                                         int64_t team, const char *server,
                                                         server_mgmt_read_selector_t selector,
                                                         char *out, size_t cap)
{
   (void)ctx;
   (void)actor;
   (void)team;
   (void)server;
   (void)selector;
   (void)out;
   (void)cap;
   pthread_mutex_lock(&block_mu);
   callback_entered = 1;
   pthread_cond_broadcast(&block_cv);
   while (!callback_release)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   return KB_MANAGEMENT_READ_OK;
}

int db2_server_registry_list(int64_t team, db2_server_row_t *rows, int max)
{
   got_list_team = team;
   if (!rows || max < 1)
      return -1;
   memset(rows, 0, sizeof(*rows));
   snprintf(rows[0].server_id, sizeof(rows[0].server_id), "stale-row");
   snprintf(rows[0].endpoint, sizeof(rows[0].endpoint), "https://server.example:443");
   return 1;
}

int kb_mgmt_endpoint_validate(const char *endpoint)
{
   return endpoint && endpoint[0] ? 0 : -1;
}

static kb_management_health_result_t handler(void *ctx, const kb_principal_t *actor, int64_t team,
                                             const char *server)
{
   assert(ctx == (void *)0x1234);
   assert(actor && actor->authenticated);
   calls++;
   got_team = team;
   snprintf(got_server, sizeof(got_server), "%s", server);
   snprintf(got_subject, sizeof(got_subject), "%s", actor->subject);
   return next_result;
}

static kb_management_health_result_t blocking_handler(void *ctx, const kb_principal_t *actor,
                                                      int64_t team, const char *server)
{
   (void)ctx;
   (void)actor;
   (void)team;
   (void)server;
   pthread_mutex_lock(&block_mu);
   callback_entered = 1;
   pthread_cond_broadcast(&block_cv);
   while (!callback_release)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   return KB_MANAGEMENT_HEALTH_OK;
}

static int route(const char *method, const char *path, const char *query, char *out, int cap)
{
   memset(out, 0, (size_t)cap);
   return kb_http_servers_route(method, path, query, out, cap);
}

static int route_body(const char *method, const char *path, const char *query, const char *body,
                      char *out, int cap)
{
   memset(out, 0, (size_t)cap);
   return kb_http_servers_route_ex(method, path, query, body, body ? strlen(body) : 0, out, cap);
}

static void set_actor(void)
{
   kb_principal_t actor;
   memset(&actor, 0, sizeof(actor));
   actor.authenticated = 1;
   snprintf(actor.subject, sizeof(actor.subject), "operator@example.test");
   kb_reqctx_set_actor(&actor);
}

static void test_route_mapping(void)
{
   char out[512];
   static const struct
   {
      kb_management_health_result_t result;
      int status;
   } cases[] = {{KB_MANAGEMENT_HEALTH_OK, 200},          {KB_MANAGEMENT_HEALTH_NOT_FOUND, 404},
                {KB_MANAGEMENT_HEALTH_DENIED, 403},      {KB_MANAGEMENT_HEALTH_CONFLICT, 409},
                {KB_MANAGEMENT_HEALTH_UNAVAILABLE, 503}, {KB_MANAGEMENT_HEALTH_INTEGRITY, 502},
                {KB_MANAGEMENT_HEALTH_INVALID, 400}};

   assert(kb_http_servers_health_register(handler, (void *)0x1234) == 0);
   assert(kb_http_servers_health_register(handler, (void *)0x1234) == -1);
   set_actor();
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      next_result = cases[i].result;
      assert(route("GET", "/v1/servers/server-a/health", "x=1&team=42", out, sizeof(out)) ==
             cases[i].status);
   }
   assert(calls == sizeof(cases) / sizeof(cases[0]));
   assert(got_team == 42);
   assert(strcmp(got_server, "server-a") == 0);
   assert(strcmp(got_subject, "operator@example.test") == 0);
   assert(strstr(out, "invalid server health request"));
   next_result = KB_MANAGEMENT_HEALTH_OK;
   assert(route("GET", "/v1/servers/server-a/health", "team=9223372036854775807", out,
                sizeof(out)) == 200);
   assert(got_team == INT64_MAX);
   assert(kb_http_servers_health_unregister(handler, (void *)0x9999) == -1);
   assert(kb_http_servers_health_unregister(handler, (void *)0x1234) == 0);
}

static void test_rejections_and_list_isolation(void)
{
   char out[512];
   unsigned int before = calls;

   set_actor();
   assert(route("POST", "/v1/servers/server-a/health", "team=1", out, sizeof(out)) == 405);
   assert(route("GET", "/v1/servers/server-a/health", NULL, out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health", "team=0", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health",
                "team=9999999999999999999999999999999999999999", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health", "team=9223372036854775808", out,
                sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health", "team=01", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health", "team=+1", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health", "team=1&team=2", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server%2fa/health", "team=1", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server?a/health", "team=1", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server#a/health", "team=1", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/health/extra", "team=1", out, sizeof(out)) == -1);
   assert(route("GET", "/v1/servers/server-a/health", "team=1", out, sizeof(out)) == 503);
   assert(route("GET", "/v1/servers", "team=1", out, sizeof(out)) == 200);
   assert(strstr(out, "stale-row"));
   assert(scope_begins == 1 && scope_commits == 1 && scope_team == 1);
   assert(calls == before);
   assert(route("GET", "/v1/servers", "team=+01&team=2", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers", "team=01", out, sizeof(out)) == 400);
   assert(got_list_team == 1 && scope_begins == 1);
   assert(calls == before);

   next_scope_result = DB2_ERR_TENANT_DENIED;
   assert(route("GET", "/v1/servers", "team=2", out, sizeof(out)) == 403);
   next_scope_result = DB2_ERR_TENANT_NO_CONN;
   assert(route("GET", "/v1/servers", "team=2", out, sizeof(out)) == 503);
   next_scope_result = 0;
   assert(scope_begins == 3 && scope_commits == 1 && got_list_team == 1);

   kb_reqctx_clear();
   assert(route("GET", "/v1/servers/server-a/health", "team=1", out, sizeof(out)) == 401);
   assert(route("GET", "/v1/servers", "team=1", out, sizeof(out)) == 401);
}

static void test_action_route(void)
{
   char out[512];
   const char *body = "{\"agent\":\"alpha\",\"action\":\"agent.enable\"}";
   set_actor();
   assert(kb_http_servers_action_register(action_handler_fn, (void *)0x4321) == 0);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=9", body, out, sizeof(out)) ==
          200);
   assert(action_calls == 1 && !strcmp(action_body, body));
   assert(route_body("POST", "/v1/servers/server-a/actions", "x=1&team=9", body, out,
                     sizeof(out)) == 400);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=09", body, out, sizeof(out)) ==
          400);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=9&team=9", body, out,
                     sizeof(out)) == 400);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=%39", body, out, sizeof(out)) ==
          400);
   assert(route_body("GET", "/v1/servers/server-a/actions", "team=9", body, out, sizeof(out)) ==
          405);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=9",
                     "{\"action\":\"agent.run\",\"agent\":\"alpha\"}", out, sizeof(out)) == 400);
   static const char nul_body[] = "{\"action\":\"agent.enable\",\"agent\":\"alpha\"}\0trailing";
   assert(kb_http_servers_route_ex("POST", "/v1/servers/server-a/actions", "team=9", nul_body,
                                   sizeof(nul_body) - 1, out, sizeof(out)) == 400);
   assert(action_calls == 1);
   assert(kb_http_servers_action_unregister(action_handler_fn, (void *)0x4321) == 0);
   assert(route_body("POST", "/v1/servers/server-a/actions", "team=9", body, out, sizeof(out)) ==
          503);
   kb_reqctx_clear();
}

static void test_read_route(void)
{
   char out[512];
   set_actor();
   assert(route("GET", "/v1/servers/server-a/agents", "team=9", out, sizeof(out)) == 503);
   assert(strstr(out, "\"code\":\"unavailable\""));
   const char *correlation = strstr(out, "\"correlation_id\":\"");
   assert(correlation && strlen(correlation + strlen("\"correlation_id\":\"")) >= 45);
   assert(kb_http_servers_read_register(read_handler_fn, (void *)0x2468) == 0);
   assert(kb_http_servers_read_register(read_handler_fn, (void *)0x2468) == -1);
   read_result = KB_MANAGEMENT_READ_OK;
   assert(route("GET", "/v1/servers/server-a/agents", "team=9", out, sizeof(out)) == 200);
   assert(strstr(out, "\"agents\":[]") && read_calls == 1);
   assert(route("GET", "/v1/servers/server-a/config", "team=9", out, sizeof(out)) == 200);
   assert(strstr(out, "\"config\":{}") && read_calls == 2);
   assert(route("POST", "/v1/servers/server-a/config", "team=9", out, sizeof(out)) == 405);
   assert(route("POST", "/v1/servers/server-a/agents", "team=9", out, sizeof(out)) == 405);
   assert(route("GET", "/v1/servers/server-a/agents", "x=1&team=9", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/agents", "team=%39", out, sizeof(out)) == 400);
   assert(route("GET", "/v1/servers/server-a/agents", "team=9&team=9", out, sizeof(out)) == 400);
   assert(route_body("GET", "/v1/servers/server-a/agents", "team=9", "{}", out, sizeof(out)) ==
          400);
   static const struct
   {
      kb_management_read_result_t result;
      int status;
      const char *code;
   } cases[] = {{KB_MANAGEMENT_READ_INVALID, 400, "invalid_request"},
                {KB_MANAGEMENT_READ_DENIED, 403, "forbidden"},
                {KB_MANAGEMENT_READ_NOT_FOUND, 404, "not_found"},
                {KB_MANAGEMENT_READ_CONFLICT, 409, "conflict"},
                {KB_MANAGEMENT_READ_INTEGRITY, 502, "integrity"},
                {KB_MANAGEMENT_READ_UNAVAILABLE, 503, "unavailable"}};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      read_result = cases[i].result;
      assert(route("GET", "/v1/servers/server-a/agents", "team=9", out, sizeof(out)) ==
             cases[i].status);
      assert(strstr(out, cases[i].code) && strstr(out, "\"message\":"));
   }
   assert(kb_http_servers_read_unregister(read_handler_fn, (void *)0x2468) == 0);
   kb_reqctx_clear();
}

typedef struct
{
   int status;
   atomic_int done;
} thread_result_t;

static void *route_thread(void *opaque)
{
   thread_result_t *r = opaque;
   char out[256];
   set_actor();
   r->status = route("GET", "/v1/servers/server-a/health", "team=1", out, sizeof(out));
   kb_reqctx_clear();
   atomic_store(&r->done, 1);
   return NULL;
}

static void *unregister_thread(void *opaque)
{
   thread_result_t *r = opaque;
   r->status = kb_http_servers_health_unregister(blocking_handler, (void *)0x5678);
   atomic_store(&r->done, 1);
   return NULL;
}

static void *action_route_thread(void *opaque)
{
   thread_result_t *r = opaque;
   char out[256];
   set_actor();
   r->status = route_body("POST", "/v1/servers/server-a/actions", "team=1",
                          "{\"action\":\"agent.enable\",\"agent\":\"a\"}", out, sizeof(out));
   kb_reqctx_clear();
   atomic_store(&r->done, 1);
   return NULL;
}

static void *action_unregister_thread(void *opaque)
{
   thread_result_t *r = opaque;
   r->status = kb_http_servers_action_unregister(blocking_action_handler, (void *)0x8765);
   atomic_store(&r->done, 1);
   return NULL;
}

static void *read_route_thread(void *opaque)
{
   thread_result_t *r = opaque;
   char out[256];
   set_actor();
   r->status = route("GET", "/v1/servers/server-a/agents", "team=1", out, sizeof(out));
   kb_reqctx_clear();
   atomic_store(&r->done, 1);
   return NULL;
}

static void *read_unregister_thread(void *opaque)
{
   thread_result_t *r = opaque;
   r->status = kb_http_servers_read_unregister(blocking_read_handler, (void *)0x9753);
   atomic_store(&r->done, 1);
   return NULL;
}

static void test_unregister_waits_for_borrow(void)
{
   pthread_t request_tid, unregister_tid;
   thread_result_t request = {0}, unreg = {0};
   char out[256];

   callback_entered = 0;
   callback_release = 0;
   assert(kb_http_servers_health_register(blocking_handler, (void *)0x5678) == 0);
   assert(pthread_create(&request_tid, NULL, route_thread, &request) == 0);
   pthread_mutex_lock(&block_mu);
   while (!callback_entered)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_create(&unregister_tid, NULL, unregister_thread, &unreg) == 0);

   usleep(20000);
   assert(!atomic_load(&unreg.done));
   set_actor();
   assert(route("GET", "/v1/servers/server-b/health", "team=1", out, sizeof(out)) == 503);
   assert(kb_http_servers_health_register(handler, (void *)0x1234) == -1);
   kb_reqctx_clear();

   pthread_mutex_lock(&block_mu);
   callback_release = 1;
   pthread_cond_broadcast(&block_cv);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_join(request_tid, NULL) == 0);
   assert(pthread_join(unregister_tid, NULL) == 0);
   assert(atomic_load(&request.done) && request.status == 200);
   assert(atomic_load(&unreg.done) && unreg.status == 0);
   assert(kb_http_servers_health_register(handler, (void *)0x1234) == 0);
   assert(kb_http_servers_health_unregister(handler, (void *)0x1234) == 0);
}

static void test_action_unregister_waits_for_borrow(void)
{
   pthread_t request_tid, unregister_tid;
   thread_result_t request = {0}, unreg = {0};
   callback_entered = 0;
   callback_release = 0;
   assert(kb_http_servers_action_register(blocking_action_handler, (void *)0x8765) == 0);
   assert(pthread_create(&request_tid, NULL, action_route_thread, &request) == 0);
   pthread_mutex_lock(&block_mu);
   while (!callback_entered)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_create(&unregister_tid, NULL, action_unregister_thread, &unreg) == 0);
   usleep(20000);
   assert(!atomic_load(&unreg.done));
   pthread_mutex_lock(&block_mu);
   callback_release = 1;
   pthread_cond_broadcast(&block_cv);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_join(request_tid, NULL) == 0);
   assert(pthread_join(unregister_tid, NULL) == 0);
   assert(request.status == 200 && unreg.status == 0);
}

static void test_read_unregister_waits_for_borrow(void)
{
   pthread_t request_tid, unregister_tid;
   thread_result_t request = {0}, unreg = {0};
   char out[256];
   callback_entered = 0;
   callback_release = 0;
   assert(kb_http_servers_read_register(blocking_read_handler, (void *)0x9753) == 0);
   assert(pthread_create(&request_tid, NULL, read_route_thread, &request) == 0);
   pthread_mutex_lock(&block_mu);
   while (!callback_entered)
      pthread_cond_wait(&block_cv, &block_mu);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_create(&unregister_tid, NULL, read_unregister_thread, &unreg) == 0);
   usleep(20000);
   assert(!atomic_load(&unreg.done));
   set_actor();
   assert(route("GET", "/v1/servers/server-b/agents", "team=1", out, sizeof(out)) == 503);
   assert(kb_http_servers_read_register(read_handler_fn, (void *)0x2468) == -1);
   kb_reqctx_clear();
   pthread_mutex_lock(&block_mu);
   callback_release = 1;
   pthread_cond_broadcast(&block_cv);
   pthread_mutex_unlock(&block_mu);
   assert(pthread_join(request_tid, NULL) == 0);
   assert(pthread_join(unregister_tid, NULL) == 0);
   assert(request.status == 200 && unreg.status == 0);
   assert(kb_http_servers_read_register(read_handler_fn, (void *)0x2468) == 0);
   assert(kb_http_servers_read_unregister(read_handler_fn, (void *)0x2468) == 0);
}

int main(void)
{
   test_route_mapping();
   test_rejections_and_list_isolation();
   test_action_route();
   test_read_route();
   test_unregister_waits_for_borrow();
   test_action_unregister_waits_for_borrow();
   test_read_unregister_waits_for_borrow();
   kb_reqctx_clear();
   puts("kb_http_servers_health: all tests passed");
   return 0;
}

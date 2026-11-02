/* test_cli_http_transport.c — unit tests for the CLI /v1 HTTP transport
 * primitives (cli_transport_parse / cli_http_request).
 *
 * The round-trip test stands up a one-shot stub HTTP server on a Unix socket in
 * a background thread and drives the real cli_http_request client against it, so
 * the request framing and response parsing are exercised end-to-end without a
 * live aimee-server. */
#include "cli_client.h"
#include "cJSON.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef AIMEE_TEST_CONNECT_WRAPPED
#include <aimee/core/connection/socket.h>
static int tcp_connect_calls;
aimee_core_result_t __wrap_aimee_core_socket_connect_controlled(const char *host, const char *port,
                                                                unsigned flags,
                                                                const aimee_core_control_t *control,
                                                                int *out_fd)
{
   (void)host;
   (void)port;
   (void)flags;
   (void)control;
   ++tcp_connect_calls;
   *out_fd = -1;
   return AIMEE_CORE_IO_ERROR;
}
static void test_refuse_before_connect(void)
{
   cli_http_header_t header = {"aUtHoRiZaTiOn", "Bearer fixture"};
   cli_http_options_t options = {.endpoint = "tcp:198.51.100.1:1", .method = "GET", .path = "/x"};
   for (int kind = 0; kind < 4; ++kind)
   {
      options.bearer = kind == 0 ? "fixture" : NULL;
      options.headers = kind == 1 ? &header : NULL;
      options.header_count = kind == 1;
      options.path = kind == 2 ? "/x\tinvalid" : kind == 3 ? "/x\177invalid" : "/x";
      cli_http_stream_t *stream = cli_http_stream_new();
      tcp_connect_calls = 0;
      assert(cli_http_stream_open(stream, &options) == -1);
      assert(tcp_connect_calls == 0);
      cli_http_stream_free(stream);
   }
   /* Counter control: admitted credential-free TCP reaches the intercepted
    * connect. No test ever opens a non-loopback network connection. */
   options.path = "/x";
   options.endpoint = "tcp:127.0.0.1:1";
   cli_http_stream_t *stream = cli_http_stream_new();
   assert(cli_http_stream_open(stream, &options) == -1);
   assert(tcp_connect_calls == 1);
   cli_http_stream_free(stream);
}
#endif

/* ---- 1. transport string -> enum ---- */
static void test_transport_parse(void)
{
   assert(cli_transport_parse("socket") == CLI_TRANSPORT_SOCKET);
   assert(cli_transport_parse("http") == CLI_TRANSPORT_HTTP);
   assert(cli_transport_parse("auto") == CLI_TRANSPORT_AUTO);
   assert(cli_transport_parse("") == CLI_TRANSPORT_SOCKET);
   assert(cli_transport_parse(NULL) == CLI_TRANSPORT_SOCKET);
   assert(cli_transport_parse("bogus") == CLI_TRANSPORT_SOCKET);
   printf("  transport_parse: ok\n");
}

/* ---- 3. end-to-end round trip against a stub UDS HTTP server ---- */
typedef struct
{
   char path[128];
   const char *response;
   int delay_ms; /* hold the response this long after reading the request */
} stub_arg_t;

static const char *test_tmpdir(void)
{
   const char *tmp = getenv("TMPDIR");
   return tmp && tmp[0] ? tmp : "/tmp";
}

static void *stub_server(void *a)
{
   stub_arg_t *s = (stub_arg_t *)a;
   int srv = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(srv >= 0);
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   assert(strlen(s->path) < sizeof(addr.sun_path));
   memcpy(addr.sun_path, s->path, strlen(s->path) + 1);
   unlink(s->path);
   assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(srv, 1) == 0);
   int c = accept(srv, NULL, NULL);
   if (c >= 0)
   {
      char req[2048];
      ssize_t r = read(c, req, sizeof(req) - 1); /* drain the request */
      (void)r;
      if (s->delay_ms > 0)
         usleep((useconds_t)s->delay_ms * 1000);
      ssize_t w = write(c, s->response, strlen(s->response));
      (void)w;
      close(c);
   }
   close(srv);
   unlink(s->path);
   return NULL;
}

static cJSON *run_stub_case(const char *response, int *status_out)
{
   stub_arg_t arg = {0};
   snprintf(arg.path, sizeof(arg.path), "%s/aimee_http_test_%d.sock", test_tmpdir(), (int)getpid());
   arg.response = response;
   pthread_t th;
   assert(pthread_create(&th, NULL, stub_server, &arg) == 0);
   usleep(30000); /* let the stub bind/listen before we connect */
   int status = -123;
   cJSON *body = cli_http_request(arg.path, "GET", "/v1/health", NULL, NULL, 2000, &status);
   pthread_join(th, NULL);
   *status_out = status;
   return body;
}

static void test_http_round_trip(void)
{
   /* 200 OK with a JSON body */
   {
      const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
      int status = 0;
      cJSON *body = run_stub_case(resp, &status);
      assert(status == 200);
      assert(body != NULL);
      cJSON *ok = cJSON_GetObjectItemCaseSensitive(body, "ok");
      assert(cJSON_IsBool(ok) && cJSON_IsTrue(ok));
      cJSON_Delete(body);
   }
   /* 404 still returns the parsed error body + status */
   {
      const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
                         "Content-Length: 16\r\nConnection: close\r\n\r\n{\"error\":\"nope\"}";
      int status = 0;
      cJSON *body = run_stub_case(resp, &status);
      assert(status == 404);
      assert(body != NULL);
      cJSON *err = cJSON_GetObjectItemCaseSensitive(body, "error");
      assert(cJSON_IsString(err) && strcmp(err->valuestring, "nope") == 0);
      cJSON_Delete(body);
   }
   printf("  http_round_trip: ok\n");
}

/* A reachable server that refuses this identity is not an old server. Pin the
 * operator-facing distinction because the wrong diagnosis sends people toward
 * an upgrade when they actually need to inspect or renew enrollment. */
static void test_manifest_auth_refusal_diagnosis(void)
{
   const char *resp = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
                      "Content-Length: 21\r\nConnection: close\r\n\r\n"
                      "{\"error\":\"forbidden\"}";
   stub_arg_t arg = {0};
   snprintf(arg.path, sizeof(arg.path), "%s/aimee_manifest_test_%d.sock", test_tmpdir(),
            (int)getpid());
   arg.response = resp;
   pthread_t th;
   assert(pthread_create(&th, NULL, stub_server, &arg) == 0);
   usleep(30000);

   char endpoint[180];
   snprintf(endpoint, sizeof(endpoint), "unix:%s", arg.path);
   setenv("AIMEE_API_ENDPOINT", endpoint, 1);

   FILE *capture = tmpfile();
   assert(capture != NULL);
   int saved_stderr = dup(STDERR_FILENO);
   assert(saved_stderr >= 0);
   fflush(stderr);
   assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

   const char *verb = NULL;
   assert(cli_v1_route_for_method("anything", &verb) == NULL);
   fflush(stderr);
   assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
   close(saved_stderr);
   pthread_join(th, NULL);
   unsetenv("AIMEE_API_ENDPOINT");

   rewind(capture);
   char diagnosis[1024] = "";
   assert(fread(diagnosis, 1, sizeof(diagnosis) - 1, capture) > 0);
   fclose(capture);
   assert(strstr(diagnosis, "authorization, not version") != NULL);
   assert(strstr(diagnosis, "aimee remote status") != NULL);
   assert(strstr(diagnosis, "aimee remote enroll") != NULL);
   assert(strstr(diagnosis, "older than this client") == NULL);
   printf("  manifest_auth_refusal_diagnosis: ok\n");
}

/* Two threads resolving a route while the manifest is still in flight must both
 * see it. The MCP bridge's tool-list watcher and its request loop do exactly
 * this at startup; marking the manifest fetched before the GET returned gave the
 * second thread no route, and the bridge reported the server unavailable. The
 * stub serves one connection, so a second fetch would also fail the lookup. */
static void *resolve_mcp_call(void *out)
{
   const char *verb = NULL;
   *(const char **)out = cli_v1_route_for_method("mcp.call", &verb);
   return NULL;
}

static void test_manifest_concurrent_first_fetch(void)
{
   cli_v1_manifest_reset_for_test();
   char home[160];
   snprintf(home, sizeof(home), "%s/aimee_manifest_home_%d", test_tmpdir(), (int)getpid());
   setenv("AIMEE_HOME", home, 1);

   static const char body[] = "{\"manifest_version\":1,\"routes\":["
                              "{\"op\":\"mcp.call\",\"verb\":\"POST\",\"path\":\"/v1/mcp/call\"}]}";
   char resp[512];
   snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            strlen(body), body);
   stub_arg_t arg = {0};
   snprintf(arg.path, sizeof(arg.path), "%s/aimee_manifest_race_%d.sock", test_tmpdir(),
            (int)getpid());
   arg.response = resp;
   arg.delay_ms = 300;
   pthread_t server;
   assert(pthread_create(&server, NULL, stub_server, &arg) == 0);
   usleep(30000);
   char endpoint[180];
   snprintf(endpoint, sizeof(endpoint), "unix:%s", arg.path);
   setenv("AIMEE_API_ENDPOINT", endpoint, 1);

   const char *first = NULL, *second = NULL;
   pthread_t a, b;
   assert(pthread_create(&a, NULL, resolve_mcp_call, &first) == 0);
   usleep(50000); /* the first lookup's GET is now held by the stub */
   assert(pthread_create(&b, NULL, resolve_mcp_call, &second) == 0);
   pthread_join(a, NULL);
   pthread_join(b, NULL);
   pthread_join(server, NULL);
   unsetenv("AIMEE_API_ENDPOINT");
   unsetenv("AIMEE_HOME");

   assert(first && strcmp(first, "/v1/mcp/call") == 0);
   assert(second && strcmp(second, "/v1/mcp/call") == 0);
   cli_v1_manifest_reset_for_test();
   printf("  manifest_concurrent_first_fetch: ok\n");
}

/* ---- method -> /v1 route lookup, over a server-supplied manifest ---- */
static void test_v1_route_map(void)
{
   const char *verb = NULL;

   /* Which method maps to which path is the SERVER's property now: the client
    * fetches the map (GET /v1/cli/manifest) instead of carrying one. Asserting
    * specific paths here would re-create the second copy this removed, and it is
    * already covered where the map lives -- server-api-conformance-check and the
    * route descriptor.
    *
    * What is still the client's, and what this covers, is the LOOKUP: the
    * sync/async split, the verb, and unknown methods resolving to nothing. */
   cJSON *doc = cJSON_Parse("{\"manifest_version\":1,\"routes\":["
                            "{\"op\":\"thing.get\",\"verb\":\"GET\",\"path\":\"/v1/thing\"},"
                            "{\"op\":\"thing.put\",\"verb\":\"POST\",\"path\":\"/v1/thing/put\"},"
                            "{\"op\":\"slow.build\",\"verb\":\"POST\",\"path\":\"/v1/slow/build\","
                            "\"async\":true}]}");
   assert(doc);
   cli_v1_manifest_set_for_test(doc);

   assert(strcmp(cli_v1_route_for_method("thing.get", &verb), "/v1/thing") == 0);
   assert(strcmp(verb, "GET") == 0);
   assert(strcmp(cli_v1_route_for_method("thing.put", &verb), "/v1/thing/put") == 0);
   assert(strcmp(verb, "POST") == 0);

   /* The async split: a queued row is NOT a synchronous route and vice versa, so
    * a caller cannot accidentally treat a run handle as a final response. */
   assert(cli_v1_route_for_method("slow.build", &verb) == NULL);
   assert(strcmp(cli_v1_async_route_for_method("slow.build", &verb), "/v1/slow/build") == 0);
   assert(strcmp(verb, "POST") == 0);
   assert(cli_v1_async_route_for_method("thing.get", &verb) == NULL);

   /* {id}-bearing routes still resolve through the path-id map, which is a
    * different marshalling contract and is not carried in the manifest. */
   const char *suffix = NULL, *id_field = NULL;
   assert(strcmp(cli_v1_pathid_route_for_method("workspace.get", &verb, &suffix, &id_field),
                 "/v1/workspaces/") == 0 &&
          strcmp(verb, "GET") == 0 && strcmp(suffix, "") == 0 && id_field == NULL);
   assert(strcmp(cli_v1_pathid_route_for_method("workspace.remove", &verb, &suffix, &id_field),
                 "/v1/workspaces/") == 0 &&
          strcmp(verb, "DELETE") == 0);
   /* A mid-{id} suffix route carries the id in a named field. */
   assert(strcmp(cli_v1_pathid_route_for_method("session.attach", &verb, &suffix, &id_field),
                 "/v1/sessions/") == 0 &&
          strcmp(verb, "POST") == 0 && strcmp(suffix, "/attach") == 0 &&
          strcmp(id_field, "session_id") == 0);
   assert(cli_v1_pathid_route_for_method("memory.search", &verb, &suffix, &id_field) == NULL);

   /* Unknown / unmapped methods return NULL with verb defaulted to POST. */
   verb = NULL;
   assert(cli_v1_route_for_method("not.a.real.method", &verb) == NULL);
   assert(verb && strcmp(verb, "POST") == 0);
   assert(cli_v1_route_for_method(NULL, NULL) == NULL);
   assert(cli_v1_route_for_method("", NULL) == NULL);

   printf("  v1_route_map: ok\n");
}

/* ---- dispatch rows: served ones decide which commands exist ---- */
static void test_served_dispatch(void)
{
   /* A cmd/verb pair this build has never contained. If a served row cannot
    * introduce one, moving the rows to the server bought nothing: a capability
    * added server-side would still need a client rebuild to be invokable. */
   cJSON *doc = cJSON_Parse("{\"manifest_version\":1,\"routes\":[],\"dispatch\":["
                            "{\"cmd\":\"brandnew\",\"sub\":\"thing\","
                            "\"method\":\"brandnew.thing\",\"timeout_ms\":1234},"
                            "{\"cmd\":\"solo\",\"method\":\"solo.any\"},"
                            "{\"cmd\":\"bare\",\"sub\":\"\",\"method\":\"bare.none\"}]}");
   assert(doc);
   cli_v1_manifest_set_for_test(doc);

   cli_v1_route_t r;
   char *argv1[] = {"thing"};
   assert(cli_v1_lookup("brandnew", 1, argv1, &r) == 1);
   assert(strcmp(r.method, "brandnew.thing") == 0);
   assert(r.timeout_ms == 1234);

   /* An ABSENT "sub" is the wildcard: it matches whatever the first arg is. */
   char *argv2[] = {"anything-at-all"};
   assert(cli_v1_lookup("solo", 1, argv2, &r) == 1);
   assert(strcmp(r.method, "solo.any") == 0);

   /* A present-but-EMPTY "sub" matches only when no subcommand was given. The
    * two are different states and collapsing them silently changes matching. */
   assert(cli_v1_lookup("bare", 0, NULL, &r) == 1);
   assert(strcmp(r.method, "bare.none") == 0);
   char *argv3[] = {"unexpected"};
   assert(cli_v1_lookup("bare", 1, argv3, &r) == 0);

   /* Served rows REPLACE the compiled-in set rather than adding to it: a client
    * that kept preferring its own build would still hide new commands, which is
    * the coupling this removes. `memory search` is in the built-in table and
    * absent from the manifest above. */
   char *argv4[] = {"search"};
   assert(cli_v1_lookup("memory", 1, argv4, &r) == 0);

   printf("  served_dispatch: ok\n");
}

/* ---- served argument shapes ---- */
static void test_served_marshal(void)
{
   /* Only an explicit "args":"none" may make the client send an empty body. A
    * looser reading would send {} for a command that needed arguments, which
    * fails at the server with a worse message than the client's own refusal. */
   cJSON *doc = cJSON_Parse("{\"manifest_version\":1,\"routes\":[],\"marshal\":["
                            "{\"method\":\"brandnew.thing\",\"args\":\"none\"},"
                            "{\"method\":\"needs.args\",\"args\":\"positional\"},"
                            "{\"method\":\"no.shape\"}]}");
   assert(doc);
   cli_v1_manifest_set_for_test(doc);

   assert(cli_v1_manifest_method_takes_no_args("brandnew.thing") == 1);
   /* A shape this client does not understand leaves the decision to its own
    * marshaller rather than guessing an empty body. */
   assert(cli_v1_manifest_method_takes_no_args("needs.args") == 0);
   assert(cli_v1_manifest_method_takes_no_args("no.shape") == 0);
   assert(cli_v1_manifest_method_takes_no_args("never.mentioned") == 0);
   assert(cli_v1_manifest_method_takes_no_args(NULL) == 0);
   assert(cli_v1_manifest_method_takes_no_args("") == 0);

   printf("  served_marshal: ok\n");
}

int main(void)
{
   printf("cli_http_transport:\n");
   test_transport_parse();
#ifdef AIMEE_TEST_CONNECT_WRAPPED
   test_refuse_before_connect();
#endif
   test_manifest_auth_refusal_diagnosis();
   test_manifest_concurrent_first_fetch();
   test_v1_route_map();
   test_served_dispatch();
   test_served_marshal();
   test_http_round_trip();
   printf("All cli_http_transport tests passed.\n");
   return 0;
}

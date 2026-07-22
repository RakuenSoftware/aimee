/* test_cli_http_transport.c — unit tests for the CLI /v1 HTTP transport
 * primitives (cli_transport_parse / cli_http_build_request / cli_http_request).
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

/* ---- 2. request builder ---- */
static void test_build_request(void)
{
   char buf[1024];
   int n = cli_http_build_request("GET", "/v1/models", NULL, NULL, NULL, buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "GET /v1/models HTTP/1.1\r\n"));
   assert(strstr(buf, "Host: localhost\r\n"));
   assert(strstr(buf, "Content-Length: 0\r\n"));
   assert(strstr(buf, "Connection: close\r\n\r\n"));
   assert(strstr(buf, "Authorization:") == NULL);

   const char *body = "{\"x\":1}"; /* strlen == 7 */
   n = cli_http_build_request("POST", "/v1/runs", "example", "tok123", body, buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "POST /v1/runs HTTP/1.1\r\n"));
   assert(strstr(buf, "Host: example\r\n"));
   assert(strstr(buf, "Authorization: Bearer tok123\r\n"));
   assert(strstr(buf, "Content-Length: 7\r\n"));
   assert(strstr(buf, "\r\n\r\n{\"x\":1}"));
   assert((size_t)n == strlen(buf));

   /* too-small buffer -> -1, no overflow */
   char tiny[8];
   assert(cli_http_build_request("GET", "/v1/models", NULL, NULL, NULL, tiny, sizeof(tiny)) == -1);
   printf("  build_request: ok\n");
}

/* ---- 3. end-to-end round trip against a stub UDS HTTP server ---- */
typedef struct
{
   char path[128];
   const char *response;
} stub_arg_t;

static void *stub_server(void *a)
{
   stub_arg_t *s = (stub_arg_t *)a;
   int srv = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(srv >= 0);
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->path);
   unlink(s->path);
   assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(srv, 1) == 0);
   int c = accept(srv, NULL, NULL);
   if (c >= 0)
   {
      char req[2048];
      ssize_t r = read(c, req, sizeof(req) - 1); /* drain the request */
      (void)r;
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
   stub_arg_t arg;
   snprintf(arg.path, sizeof(arg.path), "/tmp/aimee_http_test_%d.sock", (int)getpid());
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

/* ---- 4. streaming round trip: cli_http_request_stream parses SSE events ---- */
static int count_cb(cJSON *event, void *userdata)
{
   (void)event;
   (*(int *)userdata)++;
   return 0;
}

static void test_http_stream(void)
{
   const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                      "data: {\"i\":1}\n\ndata: {\"i\":2}\n\ndata: [DONE]\n\n";
   stub_arg_t arg;
   snprintf(arg.path, sizeof(arg.path), "/tmp/aimee_http_stream_%d.sock", (int)getpid());
   arg.response = resp;
   pthread_t th;
   assert(pthread_create(&th, NULL, stub_server, &arg) == 0);
   usleep(30000);

   int n = 0, status = 0;
   cJSON *last = cli_http_request_stream(arg.path, "POST", "/v1/chat/completions", "{}", NULL, 2000,
                                         &status, count_cb, &n);
   pthread_join(th, NULL);

   assert(status == 200);
   assert(n == 2);
   assert(last != NULL);
   cJSON *i = cJSON_GetObjectItemCaseSensitive(last, "i");
   assert(cJSON_IsNumber(i) && i->valueint == 2);
   cJSON_Delete(last);
   printf("  http_stream: ok\n");
}

/* ---- method -> first-class /v1 REST route map ---- */
static void test_v1_route_map(void)
{
   const char *verb = NULL;

   /* POST routes carry the full body. */
   assert(strcmp(cli_v1_route_for_method("index.find", &verb), "/v1/index/find") == 0);
   assert(strcmp(verb, "POST") == 0);
   assert(strcmp(cli_v1_route_for_method("memory.store", &verb), "/v1/memory/store") == 0);
   assert(strcmp(cli_v1_route_for_method("workspace.add", &verb), "/v1/workspaces") == 0 &&
          strcmp(verb, "POST") == 0);
   assert(strcmp(verb, "POST") == 0);
   assert(strcmp(cli_v1_route_for_method("collab_rules.approve", &verb),
                 "/v1/collab_rules/approve") == 0);
   assert(strcmp(verb, "POST") == 0);

   /* No-argument reads map to GET. */
   assert(strcmp(cli_v1_route_for_method("hud.status", &verb), "/v1/hud") == 0);
   assert(strcmp(verb, "GET") == 0);
   assert(strcmp(cli_v1_route_for_method("collab_rules.list", &verb), "/v1/collab_rules") == 0);
   assert(strcmp(verb, "GET") == 0);

   /* Param-bearing GET reads are mapped too: the client now sends the body on
    * GET (the server reads it via Content-Length), so filters survive. */
   assert(strcmp(cli_v1_route_for_method("skill.list", &verb), "/v1/skills") == 0 &&
          strcmp(verb, "GET") == 0);
   /* {id}-bearing path routes resolve via the path-id map, not cli_v1_route_for_method. */
   assert(cli_v1_route_for_method("workspace.get", &verb) == NULL);
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

   /* delegate runs over POST /v1/delegate/run (forced background remotely). */
   assert(strcmp(cli_v1_route_for_method("delegate", &verb), "/v1/delegate/run") == 0 &&
          strcmp(verb, "POST") == 0);

   /* Unknown / unmapped methods return NULL with verb defaulted to POST. */
   verb = NULL;
   assert(cli_v1_route_for_method("not.a.real.method", &verb) == NULL);
   assert(verb && strcmp(verb, "POST") == 0);
   assert(cli_v1_route_for_method(NULL, NULL) == NULL);
   assert(cli_v1_route_for_method("", NULL) == NULL);

   printf("  v1_route_map: ok\n");
}

int main(void)
{
   printf("cli_http_transport:\n");
   test_transport_parse();
   test_build_request();
   test_v1_route_map();
   test_http_round_trip();
   test_http_stream();
   printf("All cli_http_transport tests passed.\n");
   return 0;
}

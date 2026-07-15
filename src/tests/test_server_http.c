/* test_server_http.c: unit tests for the aimee-server /v1 persona routes and
 * the per-session persona store (no socket I/O). */
#include "server_http.h"
#include "server.h" /* CAP_* / CAPS_* bits, server_capability_for_method */
#include "openai_runs_store.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include <netinet/in.h> /* INADDR_ANY / INADDR_LOOPBACK for the bind-policy test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Stub completion handler: proves the route dispatches to a registered handler
 * and passes the body through, without linking the real inference stack. */
static int stub_completion_handler(const char *body, char *resp, int cap)
{
   int has_msg = (body && strstr(body, "\"messages\"")) ? 1 : 0;
   snprintf(resp, (size_t)cap, "{\"object\":\"chat.completion\",\"stub\":true,\"saw_messages\":%s}",
            has_msg ? "true" : "false");
   return 200;
}

/* Stub rules provider: returns a fixed heap JSON body (route frees it). */
static char *stub_rules_provider(void)
{
   return strdup("{\"epoch\":3,\"rules\":[{\"id\":\"r1\"}]}");
}

/* Stub models provider: appends two fixed agent names to /v1/models. */
static int stub_models_provider(char ids[][SERVER_HTTP_MODEL_ID_MAX], int max)
{
   if (max < 2)
      return 0;
   snprintf(ids[0], SERVER_HTTP_MODEL_ID_MAX, "claude");
   snprintf(ids[1], SERVER_HTTP_MODEL_ID_MAX, "gpt");
   return 2;
}

/* Dispatch-backed first-class /v1 routes in server_http.o reference
 * server_dispatch() and server_active_ctx() (server.c / server_main.c, not
 * linked into this test). Stub them for linking. */
/* Last dispatch captured by the stub, so route→method tests can assert which
 * NDJSON method a first-class /v1 route actually dispatched, and that the body
 * survived the bridge. */
static _Thread_local char g_disp_method[96];
static _Thread_local char g_disp_body[24576];
static char g_agg_body[24576];

int server_dispatch(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t msg_len)
{
   (void)ctx;
   /* Capture the dispatched method + body (msg is the NUL-terminated line the
    * loopback dispatch route built). */
   snprintf(g_disp_body, sizeof(g_disp_body), "%.*s", (int)msg_len, msg ? msg : "");
   if (strstr(g_disp_body, "\"method\":\"delegate.aggregate\""))
      snprintf(g_agg_body, sizeof(g_agg_body), "%s", g_disp_body);
   g_disp_method[0] = '\0';
   const char *p = strstr(g_disp_body, "\"method\":\"");
   if (p)
   {
      p += strlen("\"method\":\"");
      const char *q = strchr(p, '"');
      if (q && (size_t)(q - p) < sizeof(g_disp_method))
         snprintf(g_disp_method, sizeof(g_disp_method), "%.*s", (int)(q - p), p);
   }
   /* Mimic a real method handler: write an NDJSON response to the loopback fd
    * the first-class /v1 route handed us, so the capture path is exercised end to
    * end. */
   const char *r = "{\"status\":\"ok\",\"result\":42}\n";
   ssize_t w = write(conn->fd, r, strlen(r));
   (void)w;
   return 0;
}
server_ctx_t *server_active_ctx(void)
{
   return NULL;
}

int main(void)
{
   printf("server_http: ");

   char home[PATH_MAX];
   snprintf(home, sizeof(home), "%s/aimee-shttp-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("AIMEE_HOME", home);

   char resp[8192];

   /* --- GET /v1/health is a liveness probe --- */
   {
      int st = server_http_route("GET", "/v1/health", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"status\":\"ok\""));
      assert(strstr(resp, "\"service\":\"aimee-server\""));
   }

   /* --- GET /v1/version reports the build version --- */
   {
      int st = server_http_route("GET", "/v1/version", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"version\":\""));
      assert(strstr(resp, "\"service\":\"aimee-server\""));
   }

   /* --- GET /v1/capabilities advertises the served resources --- */
   {
      int st = server_http_route("GET", "/v1/capabilities", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"personas\"") && strstr(resp, "\"sessions\""));
      assert(strstr(resp, "\"models\""));
      assert(strstr(resp, "\"version\":\""));
   }

   /* --- GET /v1/models is an OpenAI-shaped model list with the aimee model --- */
   {
      int st = server_http_route("GET", "/v1/models", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"object\":\"list\""));
      assert(strstr(resp, "\"id\":\"aimee\""));
      assert(strstr(resp, "\"object\":\"model\""));
      /* No provider registered -> aimee only */
      assert(!strstr(resp, "\"id\":\"claude\""));
   }

   /* --- a registered models provider appends agent names --- */
   {
      server_http_set_models_provider(stub_models_provider);
      int st = server_http_route("GET", "/v1/models", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"id\":\"aimee\""));
      assert(strstr(resp, "\"id\":\"claude\"") && strstr(resp, "\"id\":\"gpt\""));
      server_http_set_models_provider(NULL);
   }

   /* --- /v1/capabilities now advertises chat --- */
   {
      int st = server_http_route("GET", "/v1/capabilities", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"chat\"") && strstr(resp, "\"embeddings\""));
   }

   /* --- POST /v1/chat/completions returns 503 until a handler is wired in --- */
   {
      int st = server_http_route("POST", "/v1/chat/completions", "{}", 2, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"error\"") && strstr(resp, "\"type\""));
   }

   /* --- with a handler registered, the route dispatches and passes the body --- */
   {
      server_http_set_chat_handler(stub_completion_handler);
      const char *body = "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
      int st = server_http_route("POST", "/v1/chat/completions", body, (int)strlen(body), resp,
                                 sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      assert(strstr(resp, "\"saw_messages\":true"));
      server_http_set_chat_handler(NULL);
   }

   /* --- /v1/completions shares the same seam --- */
   {
      int st = server_http_route("POST", "/v1/completions", "{}", 2, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_completion_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/completions", "{}", 2, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_completion_handler(NULL);
   }

   /* --- /v1/embeddings shares the same seam --- */
   {
      int st = server_http_route("POST", "/v1/embeddings", "{}", 2, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_embeddings_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/embeddings", "{}", 2, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_embeddings_handler(NULL);
   }

   /* --- /v1/responses shares the same seam --- */
   {
      int st = server_http_route("POST", "/v1/responses", "{}", 2, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_responses_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/responses", "{}", 2, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_responses_handler(NULL);
   }

   /* --- POST /v1/runs: 503 until wired; GET /v1/runs/{id} from the store --- */
   {
      int st = server_http_route("POST", "/v1/runs", "{\"input\":\"x\"}", 13, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_runs_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/runs", "{\"input\":\"x\"}", 13, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_runs_handler(NULL);

      openai_runs_store_reset();
      st = server_http_route("GET", "/v1/runs/run_xyz", NULL, 0, resp, sizeof(resp));
      assert(st == 404); /* unknown run */
      openai_runs_store_create("run_xyz", "{\"id\":\"run_xyz\",\"object\":\"run\"}");
      st = server_http_route("GET", "/v1/runs/run_xyz", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"object\":\"run\""));

      /* POST /v1/runs/{id}/stop requests cancel and returns the run, 404 when unknown */
      st = server_http_route("POST", "/v1/runs/run_xyz/stop", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"object\":\"run\""));
      st = server_http_route("POST", "/v1/runs/nope/stop", NULL, 0, resp, sizeof(resp));
      assert(st == 404);

      /* /events is served by the SSE path in handle_conn, not this buffered
       * router, so server_http_route does not match it (404 here). */
      st = server_http_route("GET", "/v1/runs/run_xyz/events", NULL, 0, resp, sizeof(resp));
      assert(st == 404);
      openai_runs_store_reset();
   }

   /* --- service routes are GET-only --- */
   {
      int st = server_http_route("POST", "/v1/health", NULL, 0, resp, sizeof(resp));
      assert(st == 404);
   }

   /* --- GET /v1/personas lists built-ins --- */
   {
      int st = server_http_route("GET", "/v1/personas", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"engineer\"") && strstr(resp, "\"novel\"") &&
             strstr(resp, "\"songwriter\""));
   }

   /* --- GET /v1/personas/<name> resolves metadata --- */
   {
      int st = server_http_route("GET", "/v1/personas/novel", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"check_role\":\"continuity\""));
      assert(strstr(resp, "\"check_marker\":\"CONTINUITY\""));
      assert(strstr(resp, "\"continuity\"")); /* in roles array */
   }

   /* --- unknown persona -> 404 --- */
   {
      int st = server_http_route("GET", "/v1/personas/does-not-exist", NULL, 0, resp, sizeof(resp));
      assert(st == 404);
   }

   /* --- session persona store: set/get + isolation --- */
   {
      char got[64];
      assert(session_persona_get("sess-A", got, sizeof(got)) == 0);

      int st = server_http_route("POST", "/v1/sessions/sess-A/persona", "{\"name\":\"novel\"}", 16,
                                 resp, sizeof(resp));
      assert(st == 200);
      assert(session_persona_get("sess-A", got, sizeof(got)) == 1);
      assert(strcmp(got, "novel") == 0);

      /* a different session is independent */
      server_http_route("POST", "/v1/sessions/sess-B/persona", "{\"name\":\"songwriter\"}", 21,
                        resp, sizeof(resp));
      assert(session_persona_get("sess-B", got, sizeof(got)) == 1 &&
             strcmp(got, "songwriter") == 0);
      assert(session_persona_get("sess-A", got, sizeof(got)) == 1 && strcmp(got, "novel") == 0);

      /* GET reads the session's persona back */
      int st2 =
          server_http_route("GET", "/v1/sessions/sess-A/persona", NULL, 0, resp, sizeof(resp));
      assert(st2 == 200 && strstr(resp, "\"name\":\"novel\""));

      /* GET for an unset session still resolves (falls back to durable default) */
      st2 = server_http_route("GET", "/v1/sessions/sess-none/persona", NULL, 0, resp, sizeof(resp));
      assert(st2 == 200 && strstr(resp, "\"name\":\""));
   }

   /* --- session primary-agent store: set/get + isolation + GET route --- */
   {
      char got[64];
      assert(session_primary_get("psess-A", got, sizeof(got)) == 0);

      session_primary_set("psess-A", "minimax");
      assert(session_primary_get("psess-A", got, sizeof(got)) == 1);
      assert(strcmp(got, "minimax") == 0);

      /* a different session is independent */
      session_primary_set("psess-B", "mistral");
      assert(session_primary_get("psess-B", got, sizeof(got)) == 1 && strcmp(got, "mistral") == 0);
      assert(session_primary_get("psess-A", got, sizeof(got)) == 1 && strcmp(got, "minimax") == 0);

      /* GET route reads the pinned primary back as JSON */
      int st =
          server_http_route("GET", "/v1/sessions/psess-A/primary", NULL, 0, resp, sizeof(resp));
      assert(st == 200 && strstr(resp, "\"agent\":\"minimax\""));

      /* GET for an unset session returns an empty agent */
      st = server_http_route("GET", "/v1/sessions/psess-none/primary", NULL, 0, resp, sizeof(resp));
      assert(st == 200 && strstr(resp, "\"agent\":\"\""));

      /* DELETE clears the pin; GET then reports empty and get returns 0 */
      st = server_http_route("DELETE", "/v1/sessions/psess-A/primary", NULL, 0, resp, sizeof(resp));
      assert(st == 200 && strstr(resp, "\"agent\":\"\""));
      assert(session_primary_get("psess-A", got, sizeof(got)) == 0);
      session_primary_clear("psess-B"); /* direct clear API */
      assert(session_primary_get("psess-B", got, sizeof(got)) == 0);
   }

   /* --- POST unknown persona -> 404, session unchanged --- */
   {
      int st = server_http_route("POST", "/v1/sessions/sess-C/persona", "{\"name\":\"nope\"}", 15,
                                 resp, sizeof(resp));
      assert(st == 404);
      char got[64];
      assert(session_persona_get("sess-C", got, sizeof(got)) == 0);
   }

   /* --- GET /v1/persona resolves the durable default (env-driven here) --- */
   {
      char *old = getenv("AIMEE_MODE");
      char *saved = old ? strdup(old) : NULL;

      platform_unsetenv("AIMEE_MODE");
      int st = server_http_route("GET", "/v1/persona", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"name\":\"engineer\"")); /* default */

      platform_setenv("AIMEE_MODE", "novel");
      st = server_http_route("GET", "/v1/persona", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"name\":\"novel\""));
      assert(strstr(resp, "\"check_role\":\"continuity\""));

      if (saved)
      {
         platform_setenv("AIMEE_MODE", saved);
         free(saved);
      }
      else
         platform_unsetenv("AIMEE_MODE");
   }

   /* --- unknown route -> 404 --- */
   {
      int st = server_http_route("GET", "/v1/nope", NULL, 0, resp, sizeof(resp));
      assert(st == 404);
   }

   /* --- GET /v1/openapi.json|.yaml serve the embedded spec --- */
   {
      int st = server_http_route("GET", "/v1/openapi.json", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "openapi:") != NULL || strstr(resp, "aimee-server") != NULL);

      st = server_http_route("GET", "/v1/openapi.yaml", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "openapi:") != NULL || strstr(resp, "aimee-server") != NULL);
   }

   /* --- the openapi route only matches GET; other methods fall through to 404 --- */
   {
      int st = server_http_route("POST", "/v1/openapi.json", "{}", 2, resp, sizeof(resp));
      assert(st == 404);
   }

   /* --- capabilities advertises openapi + responses --- */
   {
      int st = server_http_route("GET", "/v1/capabilities", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"openapi\"") != NULL);
      assert(strstr(resp, "\"responses\"") != NULL);
   }

   /* --- GET /v1/rules: 503 until a provider is wired, then emits its JSON --- */
   {
      int st = server_http_route("GET", "/v1/rules", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_rules_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/rules", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3"));
      assert(strstr(resp, "\"id\":\"r1\""));
      server_http_set_rules_provider(NULL);
   }

   /* --- /v1/capabilities now advertises rules + kb + memory + dashboard --- */
   {
      int st = server_http_route("GET", "/v1/capabilities", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"rules\""));
      assert(strstr(resp, "\"kb\""));
      assert(strstr(resp, "\"memory\""));
      assert(strstr(resp, "\"dashboard\""));
   }

   /* --- GET /v1/dashboard/memory: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/dashboard/memory", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_dashboard_memory_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/dashboard/memory", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_dashboard_memory_provider(NULL);
   }

   /* --- GET /v1/kb/status: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/kb/status", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_kb_status_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/kb/status", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_kb_status_provider(NULL);
   }

   /* --- GET /v1/agents: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/agents", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_agents_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/agents", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_agents_provider(NULL);
   }

   /* --- GET /v1/roadmap: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/roadmap", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_roadmap_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/roadmap", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_roadmap_provider(NULL);
   }

   /* --- GET /v1/curiosity: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/curiosity", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_curiosity_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/curiosity", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_curiosity_provider(NULL);
   }

   /* --- GET /v1/notes: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/notes", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_notes_list_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/notes", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_notes_list_provider(NULL);
   }

   /* --- GET /v1/dashboard/reminders: 503 until a provider is wired, then emits --- */
   {
      int st = server_http_route("GET", "/v1/dashboard/reminders", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_dashboard_reminders_provider(stub_rules_provider);
      st = server_http_route("GET", "/v1/dashboard/reminders", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"epoch\":3")); /* stub body */
      server_http_set_dashboard_reminders_provider(NULL);
   }

   /* --- POST /v1/kb/search: 503 until a handler is wired, then dispatches --- */
   {
      int st =
          server_http_route("POST", "/v1/kb/search", "{\"query\":\"x\"}", 13, resp, sizeof(resp));
      assert(st == 503);
      server_http_set_kb_search_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/kb/search", "{\"query\":\"x\"}", 13, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_kb_search_handler(NULL);
   }

   /* --- POST /v1/memory/recall: 503 until a handler is wired, then dispatches --- */
   {
      int st = server_http_route("POST", "/v1/memory/recall", "{\"task_hint\":\"x\"}", 17, resp,
                                 sizeof(resp));
      assert(st == 503);
      server_http_set_memory_recall_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/memory/recall", "{\"task_hint\":\"x\"}", 17, resp,
                             sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_memory_recall_handler(NULL);
   }

   /* --- POST /v1/notes/search: 503 until a handler is wired, then dispatches --- */
   {
      int st = server_http_route("POST", "/v1/notes/search", "{\"query\":\"x\"}", 13, resp,
                                 sizeof(resp));
      assert(st == 503);
      server_http_set_notes_search_handler(stub_completion_handler);
      st = server_http_route("POST", "/v1/notes/search", "{\"query\":\"x\"}", 13, resp,
                             sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"stub\":true"));
      server_http_set_notes_search_handler(NULL);
   }

   /* --- server_http_authorize: UDS vs TCP + bearer + session-key rule --- */
   {
      /* UDS is always authorized regardless of token, when no session key. */
      assert(server_http_authorize(0, "", NULL, NULL, 0) == 0);
      assert(server_http_authorize(0, "secret", NULL, NULL, 0) == 0);
      assert(server_http_authorize(0, "secret", "Bearer wrong", NULL, 0) == 0);

      /* TCP with no bearer configured => 503 (TCP shouldn't be serving). */
      assert(server_http_authorize(1, "", "Bearer x", NULL, 0) == 503);
      assert(server_http_authorize(1, NULL, NULL, NULL, 0) == 503);

      /* TCP with a bearer configured: Authorization or x-api-key exact match passes. */
      assert(server_http_authorize(1, "secret", "Bearer secret", NULL, 0) == 0);
      assert(server_http_authorize(1, "secret", NULL, "secret", 0) == 0);
      assert(server_http_authorize(1, "secret", "Bearer nope", "secret", 0) == 0);
      assert(server_http_authorize(1, "secret", NULL, "nope", 0) == 401);
      assert(server_http_authorize(1, "secret", NULL, NULL, 0) == 401);
      assert(server_http_authorize(1, "secret", "secret", NULL, 0) == 401);
      assert(server_http_authorize(1, "secret", "Bearer ", NULL, 0) == 401);

      /* Session-scoping key without a bearer configured => 503 on any transport. */
      assert(server_http_authorize(0, "", NULL, NULL, 1) == 503);
      assert(server_http_authorize(0, NULL, NULL, NULL, 1) == 503);
      assert(server_http_authorize(1, "", NULL, NULL, 1) == 503);
      /* With a bearer configured, the session key alone doesn't block UDS. */
      assert(server_http_authorize(0, "secret", NULL, NULL, 1) == 0);
   }

   /* --- server_http_bootstrap_gate: the one-time bootstrap bearer may ONLY
    *     rotate itself; every other TCP route is refused until it is rotated. --- */
   {
      unsetenv("AIMEE_API_BEARER_TOKEN"); /* TOFU active */
      const char *BOOT = "aimee-local-dev";
      /* Bootstrap still live: real routes refused (1), rotate_bearer allowed (0). */
      assert(server_http_bootstrap_gate(1, BOOT, "GET", "/v1/config") == 1);
      assert(server_http_bootstrap_gate(1, BOOT, "POST", "/v1/config/set") == 1);
      assert(server_http_bootstrap_gate(1, BOOT, "POST", "/v1/api/rotate_bearer") == 0);
      /* GET on the rotate path is not the rotate op -> still refused. */
      assert(server_http_bootstrap_gate(1, BOOT, "GET", "/v1/api/rotate_bearer") == 1);
      /* UDS is exempt (local trust). */
      assert(server_http_bootstrap_gate(0, BOOT, "GET", "/v1/config") == 0);
      /* Once rotated to a strong bearer, the gate is off for every route. */
      assert(server_http_bootstrap_gate(1, "deadbeef-strong-token", "GET", "/v1/config") == 0);
      /* Operator-pinned bearer opts out of TOFU even if it equals the bootstrap. */
      setenv("AIMEE_API_BEARER_TOKEN", BOOT, 1);
      assert(server_http_bootstrap_gate(1, BOOT, "GET", "/v1/config") == 0);
      unsetenv("AIMEE_API_BEARER_TOKEN");
   }

   /* --- typed SSE framing: embedded newlines become repeated data: lines --- */
   {
      char frame[256];
      int n = server_http_sse_event_format("delta", "{\"a\":1}\n{\"b\":2}", frame, sizeof(frame));
      assert(n == (int)strlen("event: delta\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n"));
      assert(strcmp(frame, "event: delta\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n") == 0);
   }

   /* --- per-route capability matrix (pure helpers) --- */
   {
      const uint32_t scoped = CAPS_READ_ONLY & ~(uint32_t)CAP_CHAT;

      /* Public routes require no capabilities. */
      assert(server_http_route_caps("GET", "/v1/health") == 0);
      assert(server_http_route_caps("GET", "/v1/version") == 0);
      assert(server_http_route_caps("GET", "/v1/capabilities") == 0);
      assert(server_http_route_caps("GET", "/v1/models") == 0);
      assert(server_http_route_caps("GET", "/v1/openapi.json") == 0);

      /* Each route's caps equal its NDJSON method twin. */
      assert(server_http_route_caps("GET", "/v1/rules") ==
             server_capability_for_method("rules.list"));
      assert(server_http_route_caps("POST", "/v1/memory/recall") ==
             server_capability_for_method("memory.recall"));
      assert(server_http_route_caps("POST", "/v1/chat/completions") ==
             server_capability_for_method("chat.send_stream"));

      /* Reads sit within the read-only set; compute requires CAP_CHAT. */
      assert((server_http_route_caps("GET", "/v1/rules") & ~CAPS_READ_ONLY) == 0);
      assert((server_http_route_caps("POST", "/v1/kb/search") & ~CAPS_READ_ONLY) == 0);
      assert(server_http_route_caps("POST", "/v1/embeddings") == CAP_CHAT);
      assert(server_http_route_caps("POST", "/v1/runs") == CAP_CHAT);
      assert(server_http_route_caps("POST", "/v1/runs/abc/stop") == CAP_CHAT);

      /* Session-persona: GET reads, POST mutates. */
      assert(server_http_route_caps("GET", "/v1/sessions/s1/persona") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/sessions/s1/persona") == CAP_SESSION_ADMIN);
      /* Run status / event reads are read-only. */
      assert(server_http_route_caps("GET", "/v1/runs/abc") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/runs/abc/events") == CAP_SESSION_READ);
      /* Unrecognized routes require nothing (404 in the router, as before). */
      assert(server_http_route_caps("GET", "/v1/nope") == 0);

      /* Workspace resource plane: list/get are index:read; register/remove are
       * tool:execute. (workspace-resource-plane AC #4 — capability-gated routes.) */
      assert(server_http_route_caps("GET", "/v1/workspaces") == CAP_INDEX_READ);
      assert(server_http_route_caps("GET", "/v1/workspaces/%2Fhome%2Fme%2Fp") == CAP_INDEX_READ);
      assert(server_http_route_caps("POST", "/v1/workspaces") == CAP_TOOL_EXECUTE);
      assert(server_http_route_caps("DELETE", "/v1/workspaces/%2Fhome%2Fme%2Fp") ==
             CAP_TOOL_EXECUTE);
      /* A read-scoped bearer (index:read only) satisfies the GETs but is denied
       * the writes; a write bearer (tool:execute) is allowed. The route gate is
       * (route_caps & conn_caps) == route_caps. */
      {
         uint32_t read_caps = CAP_INDEX_READ;
         uint32_t write_caps = CAP_TOOL_EXECUTE;
         uint32_t get_need = server_http_route_caps("GET", "/v1/workspaces");
         uint32_t post_need = server_http_route_caps("POST", "/v1/workspaces");
         uint32_t del_need = server_http_route_caps("DELETE", "/v1/workspaces/%2Fp");
         assert((get_need & read_caps) == get_need);    /* read bearer: GET allowed */
         assert((post_need & read_caps) != post_need);  /* read bearer: POST denied */
         assert((del_need & read_caps) != del_need);    /* read bearer: DELETE denied */
         assert((post_need & write_caps) == post_need); /* write bearer: POST allowed */
         assert((del_need & write_caps) == del_need);   /* write bearer: DELETE allowed */
      }
      /* The mutating workspace routes are NOT local-UDS-only — a detached client
       * registers/removes over TCP (gated by the write capability above). */
      assert(server_http_route_is_local_only("POST", "/v1/workspaces") == 0);
      assert(server_http_route_is_local_only("DELETE", "/v1/workspaces/%2Fp") == 0);

      /* Detached-runner reverse channel: tool:execute, TCP-reachable (the
       * serving client drives it remotely). An unscoped TCP bearer holds
       * CAP_TOOL_EXECUTE (CAPS_AUTHENTICATED) so it is allowed; a scoped
       * read-only bearer is not. */
      assert(server_http_route_caps("POST", "/v1/runner/poll") == CAP_TOOL_EXECUTE);
      assert(server_http_route_caps("POST", "/v1/runner/respond") == CAP_TOOL_EXECUTE);
      assert(server_http_route_is_local_only("POST", "/v1/runner/poll") == 0);
      assert(server_http_route_is_local_only("POST", "/v1/runner/respond") == 0);
      assert(server_http_route_allowed(1, "unscoped-bearer", "POST", "/v1/runner/poll", 0) == 1);
      assert(server_http_route_allowed(1, "scope:project:a:secret", "POST", "/v1/runner/poll", 0) ==
             0);

      /* Forge-token install: tool:execute, TCP-reachable (a client hands the hub
       * its short-lived token over /v1); a scoped read-only bearer is denied. */
      assert(server_http_route_caps("POST", "/v1/workspaces/%2Fp/forge-token") == CAP_TOOL_EXECUTE);
      assert(server_http_route_is_local_only("POST", "/v1/workspaces/%2Fp/forge-token") == 0);
      assert(server_http_route_allowed(1, "scope:project:a:s", "POST",
                                       "/v1/workspaces/%2Fp/forge-token", 0) == 0);

      /* Connection effective caps by transport + bearer. */
      assert(server_http_conn_caps(0, NULL, 0) == CAPS_ALL);                /* UDS */
      assert(server_http_conn_caps(0, "scope:project:a:s", 0) == CAPS_ALL); /* UDS exempt */
      assert(server_http_conn_caps(1, NULL, 0) == CAPS_AUTHENTICATED);      /* unscoped */
      assert(server_http_conn_caps(1, "plain-token", 0) == CAPS_AUTHENTICATED);
      assert(server_http_conn_caps(1, "scope:project:alpha:s3cr3t", 0) == scoped);

      /* Scoped bearer: denied compute/write, allowed reads/queries. */
      const char *sb = "scope:project:alpha:s3cr3t";
      assert(server_http_route_allowed(1, sb, "POST", "/v1/chat/completions", 0) == 0);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/embeddings", 0) == 0);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/runs", 0) == 0);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/runs/abc/stop", 0) == 0);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/sessions/s1/persona", 0) == 0);
      assert(server_http_route_allowed(1, sb, "GET", "/v1/rules", 0) == 1);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/memory/recall", 0) == 1);
      assert(server_http_route_allowed(1, sb, "GET", "/v1/runs/abc", 0) == 1);
      assert(server_http_route_allowed(1, sb, "GET", "/v1/runs/abc/events", 0) == 1);
      assert(server_http_route_allowed(1, sb, "GET", "/v1/health", 0) == 1);

      /* Unscoped bearer: read-capability routes (chat/runs are CAP_CHAT) are
       * reachable; privileged routes are gated by remote_writes (see below). */
      assert(server_http_route_allowed(1, NULL, "POST", "/v1/chat/completions", 0) == 1);
      assert(server_http_route_allowed(1, "plain-token", "POST", "/v1/runs", 0) == 1);
      /* persona set is CAP_SESSION_ADMIN (privileged) -> local-only at remote_writes=off. */
      assert(server_http_route_allowed(1, "plain-token", "POST", "/v1/sessions/s1/persona", 0) ==
             0);
      assert(server_http_route_allowed(1, "plain-token", "POST", "/v1/sessions/s1/persona",
                                       SERVER_REMOTE_WRITES_FULL) == 1);

      /* UDS is allowed everything. */
      assert(server_http_route_allowed(0, NULL, "POST", "/v1/chat/completions", 0) == 1);
      assert(server_http_route_allowed(0, sb, "POST", "/v1/sessions/s1/persona", 0) == 1);

      /* Privileged exec/control routes (delegate/cron/agent/provider/worktree/...)
       * are local-only over TCP unless remote_writes==full; data-plane writes need
       * only remote_writes>=data. Fail-closed at the default. */
      const char *exec_paths[] = {"/v1/delegate/launch",     "/v1/delegate/backend_exec",
                                  "/v1/delegate/roundtable", "/v1/cron/add",
                                  "/v1/agent/add",           "/v1/worktree/gc",
                                  "/v1/model/refresh",       "/v1/api/disable"};
      for (size_t i = 0; i < sizeof(exec_paths) / sizeof(exec_paths[0]); i++)
      {
         assert(server_http_route_allowed(1, "plain", "POST", exec_paths[i],
                                          SERVER_REMOTE_WRITES_OFF) == 0);
         assert(server_http_route_allowed(1, "plain", "POST", exec_paths[i],
                                          SERVER_REMOTE_WRITES_DATA) == 0); /* data != full */
         assert(server_http_route_allowed(1, "plain", "POST", exec_paths[i],
                                          SERVER_REMOTE_WRITES_FULL) == 1);
         assert(server_http_route_allowed(0, NULL, "POST", exec_paths[i],
                                          SERVER_REMOTE_WRITES_OFF) == 1); /* UDS always */
      }
      assert(server_http_route_caps("POST", "/v1/delegate/roundtable") == CAP_DELEGATE);
      assert(server_http_route_allowed(1, "scope:project:alpha:s3cr3t", "POST",
                                       "/v1/delegate/roundtable", SERVER_REMOTE_WRITES_FULL) == 0);
      /* The detached-workspace plane is exempt: reachable over TCP at remote_writes=off
       * (still cap-gated -> a scoped query-only bearer is still denied). */
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/runner/poll", 0) == 1);
      /* primary select/clear inherit their method cap (CAP_SESSION_READ), not 0. */
      assert(server_http_route_caps("POST", "/v1/sessions/s1/primary") == CAP_SESSION_READ);
      assert(server_http_route_caps("DELETE", "/v1/sessions/s1/primary") == CAP_SESSION_READ);
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/runner/respond", 0) == 1);
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/workspaces", 0) == 1); /* add */
      assert(server_http_route_allowed(1, "plain", "DELETE", "/v1/workspaces/%2Ftmp", 0) == 1);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/runner/poll", 0) ==
             0); /* scoped denied */
      assert(server_http_route_allowed(1, sb, "POST", "/v1/workspaces", 0) == 0);
      assert(server_http_route_allowed(1, sb, "DELETE", "/v1/workspaces/%2Ftmp", 0) == 0);
   }

   /* --- declarative route registry: capability rows --- */
   {
      /* Dashboard reads share one cap; persona/role-template mutations are admin. */
      assert(server_http_route_caps("GET", "/v1/agents") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("GET", "/v1/dashboard/memory") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("GET", "/v1/notes") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/kb/search") == CAP_INDEX_READ);
      assert(server_http_route_caps("GET", "/v1/personas/alice") == CAP_SESSION_READ);
      assert(server_http_route_caps("PUT", "/v1/personas/alice") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("DELETE", "/v1/personas/alice") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("POST", "/v1/personas") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("GET", "/v1/role_templates") == CAP_SESSION_READ);
      assert(server_http_route_caps("DELETE", "/v1/role_templates/qa") == CAP_SESSION_ADMIN);
      /* Proposals read surfaces: the timeline + proposal-markdown reads share the
       * dashboard-read cap (ownership is enforced in-handler, not by the route cap),
       * while the operator "list all items" view requires CAP_WORKFLOW_ADMIN. The
       * /all exact row must win over the /<id> prefix row (else "all" is parsed as a
       * work-item id under CAP_DASHBOARD_READ). */
      assert(server_http_route_caps("GET", "/v1/workflow/items/wi_x/events") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("GET", "/v1/workflow/items/wi_x/proposal") ==
             CAP_DASHBOARD_READ);
      assert(server_http_route_caps("GET", "/v1/workflow/items/all") == CAP_WORKFLOW_ADMIN);
      assert(server_http_route_caps("GET", "/v1/workflow/items/wi_x") == CAP_DASHBOARD_READ);
      /* Lifecycle mutations: route cap admits owners (CAP_DASHBOARD_READ); the
       * handler re-checks owner-or-operator. The suffix rows must win over the bare
       * /<id> row, and DELETE /<id> is distinct from GET /<id> by verb. */
      assert(server_http_route_caps("POST", "/v1/workflow/items/wi_x/pause") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("POST", "/v1/workflow/items/wi_x/resume") ==
             CAP_DASHBOARD_READ);
      assert(server_http_route_caps("POST", "/v1/workflow/items/wi_x/stop") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("DELETE", "/v1/workflow/items/wi_x") == CAP_DASHBOARD_READ);
      /* Composer project-file browser (read-only). */
      assert(server_http_route_caps("GET", "/v1/workflow/repo/tree") == CAP_DASHBOARD_READ);
      assert(server_http_route_caps("GET", "/v1/workflow/repo/file") == CAP_DASHBOARD_READ);
      /* Presence is session-scoped; the streaming routes carry caps too. */
      assert(server_http_route_caps("GET", "/v1/sessions") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/sessions/s1/attach") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/sessions/s1/events") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/chat/stream") ==
             server_capability_for_method("chat.send_stream"));
      /* Verb is part of the match: a wrong-verb path is unrecognized. */
      assert(server_http_route_caps("PUT", "/v1/health") == 0);
      assert(server_http_route_caps("POST", "/v1/agents") == 0);
      /* The dynamic <id> is exactly one segment: a deeper path with no known
       * suffix matches nothing. */
      assert(server_http_route_caps("GET", "/v1/runs/a/b") == 0);

      /* Code-index read family (P1): each route inherits its NDJSON twin's cap
       * (index.* -> CAP_INDEX_READ) and stays within the read-only set. */
      assert(server_http_route_caps("POST", "/v1/index/find") ==
             server_capability_for_method("index.find"));
      assert(server_http_route_caps("POST", "/v1/index/find") == CAP_INDEX_READ);
      assert(server_http_route_caps("POST", "/v1/index/list") == CAP_INDEX_READ);
      assert(server_http_route_caps("POST", "/v1/index/structure") == CAP_INDEX_READ);
      assert(server_http_route_caps("POST", "/v1/index/find_callers") == CAP_INDEX_READ);
      assert(server_http_route_caps("POST", "/v1/index/blast_radius") == CAP_INDEX_READ);
      assert((server_http_route_caps("POST", "/v1/index/find") & ~CAPS_READ_ONLY) == 0);
      /* Wrong verb / unknown index sub-resource does not match. */
      assert(server_http_route_caps("GET", "/v1/index/find") == 0);
      assert(server_http_route_caps("POST", "/v1/index/nonesuch") == 0);
      /* index.scan is now first-class but async (rh_dispatch_op_async); it still
       * inherits its op twin's capability via the route gate. */
      assert(server_http_route_caps("POST", "/v1/index/scan") ==
             server_capability_for_method("index.scan"));
      /* Other long-running methods routed async are likewise op-cap-gated. */
      assert(server_http_route_caps("POST", "/v1/kb/build") ==
             server_capability_for_method("kb.build"));
      assert(server_http_route_caps("POST", "/v1/eval/run") ==
             server_capability_for_method("eval.run"));
      assert(server_http_route_caps("POST", "/v1/graph/sync_code") ==
             server_capability_for_method("graph.sync_code"));

      /* Skill + work read families (P1): session-read, from their op twins. */
      assert(server_http_route_caps("GET", "/v1/skills") ==
             server_capability_for_method("skill.list"));
      assert(server_http_route_caps("GET", "/v1/skills") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/skills/show") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/skills/show") == 0);

      /* HUD + trajectory read families (P1): session-read, from their op twins. */
      assert(server_http_route_caps("GET", "/v1/hud") ==
             server_capability_for_method("hud.status"));
      assert(server_http_route_caps("GET", "/v1/hud") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/trajectory/export") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/hud") == 0);

      /* Toolset / collab-rule / wm / attempt / aux read families (P1). */
      assert(server_http_route_caps("GET", "/v1/toolsets") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/toolsets/show") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/collab_rules") ==
             server_capability_for_method("collab_rules.list"));
      assert(server_http_route_caps("GET", "/v1/collab_rules") == CAP_RULES_READ);
      assert(server_http_route_caps("GET", "/v1/collab_rules/active") == CAP_RULES_READ);
      assert(server_http_route_caps("POST", "/v1/wm/list") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/attempts/list") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/aux/config") == CAP_SESSION_READ);
      /* Wrong verb does not match. */
      assert(server_http_route_caps("POST", "/v1/toolsets") == 0);
      assert(server_http_route_caps("GET", "/v1/wm/list") == 0);

      /* Memory read family (P1): memory.* reads -> CAP_MEMORY_READ. */
      assert(server_http_route_caps("POST", "/v1/memory/search") ==
             server_capability_for_method("memory.search"));
      assert(server_http_route_caps("POST", "/v1/memory/search") == CAP_MEMORY_READ);
      assert(server_http_route_caps("POST", "/v1/memory/list") == CAP_MEMORY_READ);
      assert(server_http_route_caps("GET", "/v1/memory/stats") == CAP_MEMORY_READ);
      assert(server_http_route_caps("POST", "/v1/memory/get") == CAP_MEMORY_READ);
      assert(server_http_route_caps("GET", "/v1/memory/read") == CAP_MEMORY_READ);
      /* The existing recall route is unchanged. */
      assert(server_http_route_caps("POST", "/v1/memory/recall") ==
             server_capability_for_method("memory.recall"));
   }

   /* --- data-write families default to local-UDS-only (P1) --- */
   {
      const char *sb = "scope:project:alpha:s3cr3t";
      /* Caps still derive from the op for the local path. */
      assert(server_http_route_caps("POST", "/v1/memory/store") == CAP_MEMORY_WRITE);

      /* Mutating routes are allowed on UDS (is_tcp == 0) ... */
      assert(server_http_route_allowed(0, NULL, "POST", "/v1/memory/store", 0) == 1);
      /* ... but not over TCP at the default, regardless of bearer (unscoped or scoped). */
      assert(server_http_route_allowed(1, NULL, "POST", "/v1/memory/store", 0) == 0);
      assert(server_http_route_allowed(1, "plain-token", "POST", "/v1/memory/store", 0) == 0);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/memory/store", 0) == 0);
      /* Read routes are unaffected: still reachable over TCP. */
      assert(server_http_route_allowed(1, NULL, "POST", "/v1/memory/search", 0) == 1);

      /* Later write batches: session + rules/collab-rules + skill mutations, all
       * UDS-only at the default remote_writes=off. */
      const char *write_paths[] = {"/v1/wm/set",
                                   "/v1/attempts/record",
                                   "/v1/rules/delete",
                                   "/v1/collab_rules/approve",
                                   "/v1/collab_rules/reject",
                                   "/v1/collab_rules/retire",
                                   "/v1/skills/create",
                                   "/v1/skills/edit",
                                   "/v1/skills/archive",
                                   "/v1/skills/pin"};
      for (size_t i = 0; i < sizeof(write_paths) / sizeof(write_paths[0]); i++)
      {
         assert(server_http_route_allowed(0, NULL, "POST", write_paths[i], 0) == 1); /* UDS ok */
         assert(server_http_route_allowed(1, NULL, "POST", write_paths[i], 0) == 0); /* TCP deny */
         assert(server_http_route_allowed(1, sb, "POST", write_paths[i], 0) == 0); /* scoped deny */
         assert(server_http_route_allowed(1, "plain", "POST", write_paths[i], 0) ==
                0); /* unscoped deny */
      }
      /* Caps still derive from the op for the local path. */
      assert(server_http_route_caps("POST", "/v1/rules/delete") == CAP_RULES_ADMIN);
      assert(server_http_route_caps("POST", "/v1/collab_rules/approve") == CAP_RULES_ADMIN);
      assert(server_http_route_caps("POST", "/v1/skills/create") == CAP_TOOL_WRITE);
      /* Skill reads remain TCP-reachable (only the mutations are write-gated). */
      assert(server_http_route_allowed(1, NULL, "GET", "/v1/skills", 0) == 1);
   }

   /* --- aimee.api.remote_writes lifts the TCP write deny under capability control --- */
   {
      const char *sb = "scope:project:alpha:s3cr3t";

      /* OFF (default): mutating routes denied over TCP for any bearer. */
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/memory/store",
                                       SERVER_REMOTE_WRITES_OFF) == 0);

      /* DATA: an unscoped TCP bearer reaches data-mutating routes (caps satisfied);
       * a scoped query-only bearer still cannot; reads are unchanged. */
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/memory/store",
                                       SERVER_REMOTE_WRITES_DATA) == 1);
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/rules/delete",
                                       SERVER_REMOTE_WRITES_DATA) == 1);
      assert(server_http_route_allowed(1, sb, "POST", "/v1/memory/store",
                                       SERVER_REMOTE_WRITES_DATA) == 0);
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/memory/search",
                                       SERVER_REMOTE_WRITES_DATA) == 1);

      /* conn caps by level: data keeps CAPS_AUTHENTICATED, full grants CAPS_ALL. */
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_OFF) == CAPS_AUTHENTICATED);
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_DATA) == CAPS_AUTHENTICATED);
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_FULL) == CAPS_ALL);

      /* UDS is always full, independent of the level. */
      assert(server_http_conn_caps(0, NULL, SERVER_REMOTE_WRITES_OFF) == CAPS_ALL);
   }

   /* --- declarative route registry: dispatch --- */
   {
      char rb[1024];
      /* A self-contained public route dispatches through the table. */
      assert(server_http_route("GET", "/v1/health", NULL, 0, rb, sizeof(rb)) == 200);
      assert(strstr(rb, "aimee-server"));
      /* An unknown path 404s. */
      assert(server_http_route("GET", "/v1/nope", NULL, 0, rb, sizeof(rb)) == 404);
      /* A wrong-verb known path 404s (no matching row). */
      assert(server_http_route("DELETE", "/v1/health", NULL, 0, rb, sizeof(rb)) == 404);
      /* A real route with no handler seam wired in this test returns 503, not 404
       * — proving the row matched and dispatched. */
      assert(server_http_route("GET", "/v1/rules", NULL, 0, rb, sizeof(rb)) == 503);
      openai_runs_store_reset();
      const char *roundtable_body = "{\"prompt\":\"draft\"}";
      assert(server_http_route("POST", "/v1/delegate/roundtable", roundtable_body,
                               (int)strlen(roundtable_body), rb, sizeof(rb)) == 200);
      assert(strstr(rb, "\"object\":\"op.run\""));
      assert(strstr(rb, "\"method\":\"delegate.roundtable\""));
      assert(strstr(rb, "\"status\":\"queued\""));
      for (int i = 0; i < 100 && strcmp(g_disp_method, "delegate.roundtable") != 0; i++)
         usleep(1000);
      char *large_body = malloc(9200);
      assert(large_body);
      strcpy(large_body, "{\"prompt\":\"");
      size_t prefix_len = strlen(large_body);
      memset(large_body + prefix_len, 'x', 9000);
      strcpy(large_body + prefix_len + 9000, "\"}");
      assert(server_http_route("POST", "/v1/delegate/aggregate", large_body,
                               (int)strlen(large_body), rb, sizeof(rb)) == 200);
      assert(strstr(rb, "\"object\":\"op.run\""));
      assert(strstr(rb, "\"method\":\"delegate.aggregate\""));
      char run_id[96] = "";
      char *idp = strstr(rb, "\"id\":\"");
      assert(idp);
      idp += strlen("\"id\":\"");
      char *ide = strchr(idp, '"');
      assert(ide && (size_t)(ide - idp) < sizeof(run_id));
      snprintf(run_id, sizeof(run_id), "%.*s", (int)(ide - idp), idp);
      openai_run_status_t st = OPENAI_RUN_QUEUED;
      for (int i = 0; i < 100; i++)
      {
         assert(openai_runs_store_status(run_id, &st));
         if (openai_run_status_terminal(st))
            break;
         usleep(10000);
      }
      assert(st == OPENAI_RUN_COMPLETED);
      assert(openai_runs_store_get(run_id, rb, sizeof(rb)));
      assert(strstr(rb, "\"status\":\"completed\""));
      assert(strstr(g_agg_body, "\"method\":\"delegate.aggregate\""));
      assert(strstr(g_agg_body, "\"prompt\":\"xxx"));
      free(large_body);
      g_disp_method[0] = '\0';
      g_disp_body[0] = '\0';
      openai_runs_store_reset();
      assert(server_http_submit_op_run("delegate.roundtable", "{\"prompt\":\"draft\"}",
                                       CAP_TOOL_EXECUTE, rb, sizeof(rb)) == 403);
      assert(strstr(rb, "insufficient capabilities"));
      /* The /v1/rpc bridge was retired: the path is now unrouted (404). */
      assert(server_http_route("POST", "/v1/rpc", "{}", 2, rb, sizeof(rb)) == 404);
      /* A deeper run path (two segments, no /stop|/events) does not match. */
      assert(server_http_route("GET", "/v1/runs/a/b", NULL, 0, rb, sizeof(rb)) == 404);
   }

   /* --- api status report (pure VS Code provider-snippet generator) --- */
   {
      char report[2048];

      /* Enabled listener: emits the loopback base URL, model id, and providers. */
      server_http_api_status_report(8910, 1, 60, report, sizeof(report));
      assert(strstr(report, "http://127.0.0.1:8910/v1"));
      assert(strstr(report, "model aimee"));
      assert(strstr(report, "Continue"));
      assert(strstr(report, "Copilot"));
      assert(strstr(report, "configured"));
      assert(strstr(report, "60 req/min"));
      /* Recommends a project-scoped bearer for the editor. */
      assert(strstr(report, "scope:project:<id>:<secret>"));

      /* Missing bearer is called out (the listener refuses to bind without it). */
      server_http_api_status_report(8910, 0, 0, report, sizeof(report));
      assert(strstr(report, "NOT configured"));
      assert(strstr(report, "unlimited"));

      /* Disabled listener: explains how to turn it on, no provider snippets. */
      server_http_api_status_report(0, 0, 0, report, sizeof(report));
      assert(strstr(report, "disabled"));
      assert(strstr(report, "http_port: 8910"));
      assert(!strstr(report, "http://127.0.0.1"));

      /* Never overflows a tiny buffer. */
      char tiny[16];
      server_http_api_status_report(8910, 1, 60, tiny, sizeof(tiny));
      assert(tiny[sizeof(tiny) - 1] == '\0');
   }

   /* --- server_http_rate_check: fixed 60s window, 429 + Retry-After --- */
   {
      server_http_rate_state_t st = {0, 0};

      /* limit <= 0 disables limiting entirely */
      assert(server_http_rate_check(&st, 0, 1000) == 0);
      assert(server_http_rate_check(&st, -5, 1000) == 0);

      /* limit=3: first three admitted, fourth throttled within the window */
      st.window_start = 0;
      st.count = 0;
      assert(server_http_rate_check(&st, 3, 1000) == 0);
      assert(server_http_rate_check(&st, 3, 1005) == 0);
      assert(server_http_rate_check(&st, 3, 1010) == 0);
      int retry = server_http_rate_check(&st, 3, 1015);
      assert(retry > 0 && retry <= 60); /* seconds until the window (started @1000) resets */
      assert(retry == 45);              /* 60 - (1015 - 1000) */

      /* once the window rolls over, the budget refreshes */
      assert(server_http_rate_check(&st, 3, 1061) == 0);

      /* a backwards clock jump resets the window rather than locking out */
      st.window_start = 5000;
      st.count = 3;
      assert(server_http_rate_check(&st, 3, 100) == 0);
   }

   /* --- server_http_request_id: echo provided, else generate <pid>-<seq> --- */
   {
      char rid[64];
      server_http_request_id("client-abc", 1234, 7, rid, sizeof(rid));
      assert(strcmp(rid, "client-abc") == 0); /* inbound id echoed verbatim */

      server_http_request_id("", 1234, 7, rid, sizeof(rid));
      assert(strcmp(rid, "1234-7") == 0); /* generated when absent */

      server_http_request_id(NULL, 99, 1, rid, sizeof(rid));
      assert(strcmp(rid, "99-1") == 0);

      /* truncation is safe (NUL-terminated within bounds) */
      char small[8];
      server_http_request_id("0123456789abcdef", 1, 1, small, sizeof(small));
      assert(small[sizeof(small) - 1] == '\0');
   }

   /* --- First-class op-route parity (P1/P3): a dedicated /v1/<family>/<verb>
    * route dispatches exactly its NDJSON op twin (method server-set from the
    * matched row, never the client body), preserves the request body, and echoes
    * the raw dispatch response byte-for-byte (only the trailing newline trimmed,
    * no envelope). The stub server_dispatch above writes "{...}\n"; the route body
    * must equal that NDJSON minus the newline. This is what let the /v1/rpc bridge
    * be retired — every method reaches the same dispatch surface via its route. --- */
   {
      struct
      {
         const char *verb;
         const char *path;
         const char *body;
         const char *expect_op;
      } cases[] = {
          {"POST", "/v1/cron/add", "{\"name\":\"nightly\"}", "cron.add"},
          {"POST", "/v1/provider/set", "{\"name\":\"openai\"}", "provider.set"},
          {"POST", "/v1/wm/context", "{\"k\":\"v\"}", "wm.context"},
          {"POST", "/v1/agent/add", "{\"name\":\"a\"}", "agent.add"},
          {"POST", "/v1/mcp/audit", "{}", "mcp.audit"},
          {"GET", "/v1/cron", NULL, "cron.list"},
          {"GET", "/v1/provider/list", NULL, "provider.list"},
      };
      for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      {
         g_disp_method[0] = '\0';
         g_disp_body[0] = '\0';
         int blen = cases[i].body ? (int)strlen(cases[i].body) : 0;
         int st = server_http_route(cases[i].verb, cases[i].path, cases[i].body, blen, resp,
                                    sizeof(resp));
         assert(st == 200);
         /* dispatched exactly the op twin, server-set from the row */
         assert(strcmp(g_disp_method, cases[i].expect_op) == 0);
         /* byte-identical to the route's echo of the same dispatch */
         assert(strcmp(resp, "{\"status\":\"ok\",\"result\":42}") == 0);
         /* the client body survived (a field from it appears in the dispatch) */
         if (cases[i].body && strchr(cases[i].body, ':'))
            assert(strstr(g_disp_body, "name") || strstr(g_disp_body, "\"k\"") ||
                   strstr(g_disp_body, "\"id\""));
      }
      /* A client-supplied "method" in the body cannot override the row's op. */
      g_disp_method[0] = '\0';
      const char *spoof = "{\"method\":\"server.shutdown\",\"name\":\"x\"}";
      int st =
          server_http_route("POST", "/v1/cron/add", spoof, (int)strlen(spoof), resp, sizeof(resp));
      assert(st == 200);
      assert(strcmp(g_disp_method, "cron.add") == 0);
   }

   /* (The /v1/rpc method-allowlist tests were removed with the bridge: each method
    * now has a first-class route whose per-route caps + remote_writes tier are
    * enforced by server_http_route_allowed, covered above.) */

   /* --- /v1 listener bind policy: plaintext is loopback-only, always --- */
   {
      /* The plaintext listener passes allow_external=0: a non-loopback bind is
       * refused even when AIMEE_SERVER_HTTP_BIND requests one, so the bearer can
       * never face the network in cleartext. */
      assert(server_http_resolve_bind_addr(0 /*want_ext*/, 0 /*plaintext*/) == INADDR_LOOPBACK);
      assert(server_http_resolve_bind_addr(1 /*want_ext*/, 0 /*plaintext*/) == INADDR_LOOPBACK);
      /* The TLS listener (allow_external=1) may face the network when asked. */
      assert(server_http_resolve_bind_addr(0 /*want_ext*/, 1 /*tls*/) == INADDR_LOOPBACK);
      assert(server_http_resolve_bind_addr(1 /*want_ext*/, 1 /*tls*/) == INADDR_ANY);
   }

   /* --- AIMEE_WEBCHAT_GIT=0 disables the whole git surface (503 first) --- */
   {
      char resp[2048];
      /* Every git-surface route; the gate runs before any other work, so a
       * disabled surface returns 503 for all of them (no server-ctx access). */
      static const struct
      {
         const char *m, *p, *body;
      } git_routes[] = {
          {"POST", "/v1/workspaces/ws1/forge-token", "{}"},
          {"POST", "/v1/workspace/clone", "{}"},
          {"POST", "/v1/workspace/git", "{}"},
          {"GET", "/v1/workspace/projects", NULL},
          {"POST", "/v1/workspace/projects/delete", "{}"},
          {"POST", "/v1/workspace/session-dir", "{}"},
          {"GET", "/v1/git/credentials", NULL},
          {"POST", "/v1/git/sshkey", "{}"},
          {"POST", "/v1/git/oauth/github/start", "{}"},
          {"POST", "/v1/git/oauth/github/poll", "{}"},
          {"GET", "/v1/git/oauth/github/config", NULL},
      };
      const int gn = (int)(sizeof(git_routes) / sizeof(git_routes[0]));
      setenv("AIMEE_WEBCHAT_GIT", "0", 1);
      for (int i = 0; i < gn; i++)
      {
         int blen = git_routes[i].body ? (int)strlen(git_routes[i].body) : 0;
         int st = server_http_route(git_routes[i].m, git_routes[i].p, git_routes[i].body, blen,
                                    resp, sizeof(resp));
         assert(st == 503);
      }
      /* Enabled (default + any non-"0"): the gate no longer fires. The two
       * context-free routes fall through to their own 403 webuser check (no
       * attested webuser in this harness) — crucially NOT 503. */
      unsetenv("AIMEE_WEBCHAT_GIT");
      assert(server_http_route("GET", "/v1/workspace/projects", NULL, 0, resp, sizeof(resp)) ==
             403);
      assert(server_http_route("GET", "/v1/git/credentials", NULL, 0, resp, sizeof(resp)) == 403);
      setenv("AIMEE_WEBCHAT_GIT", "1", 1);
      assert(server_http_route("GET", "/v1/workspace/projects", NULL, 0, resp, sizeof(resp)) ==
             403);
      unsetenv("AIMEE_WEBCHAT_GIT");
   }

   platform_test_rmrf(home);
   printf("OK\n");
   return 0;
}

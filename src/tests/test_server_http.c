/* test_server_http.c: unit tests for the aimee-server /v1 persona routes and
 * the per-session persona store (no socket I/O). */
#include "server_http.h"
#include "server_http_authz.h"
#include "server_http_internal.h"
#include "runtime_secret.h"
#include "http_content_encoding.h"
#include "server.h" /* CAP_* / CAPS_* bits, server_capability_for_method */
#include "server/server_mgmt_endpoint.h"
#include "server/workflow_control_bus.h"
#include <aimee/audit/obs_bus.h>
#include "agent_config.h"
#include "config.h"
#include "role_templates.h"
#include "delegate_permissions_stub.h"
#include "cJSON.h"
#include "db1.h"
#include "openai_runs_store.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "util.h"
#include <netinet/in.h> /* INADDR_ANY / INADDR_LOOPBACK for the bind-policy test */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

extern int g_remote_writes;

typedef struct
{
   pthread_barrier_t *barrier;
   int result;
   char bearer[65];
} wizard_bootstrap_thread_t;

static void *wizard_bootstrap_thread(void *arg)
{
   wizard_bootstrap_thread_t *thread = arg;
   int barrier_result = pthread_barrier_wait(thread->barrier);
   assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
   thread->result =
       server_http_first_user_bootstrap("webuser:alice", thread->bearer, sizeof(thread->bearer));
   return NULL;
}

int kb_client_mtls_management_jwks_fetch(void *ctx, char *out, size_t cap, size_t *len)
{
   (void)ctx;
   if (out && cap)
      out[0] = 0;
   if (len)
      *len = 0;
   return -1;
}

int kb_client_mtls_managed_metadata(char *server_id, size_t cap, long long *team_id)
{
   (void)server_id;
   (void)cap;
   (void)team_id;
   return 0;
}

int audit_worm_append(const char *role, const char *principal, const char *action,
                      const char *resource, const char *verdict, const char *detail)
{
   (void)role;
   (void)principal;
   (void)action;
   (void)resource;
   (void)verdict;
   (void)detail;
   return 0;
}

server_mgmt_checkpoint_result_t
server_mgmt_checkpoint_client_verify(const server_mgmt_endpoint_request_t *rq,
                                     const server_mgmt_token_claims_t *claims, uint64_t generation,
                                     const char *digest)
{
   (void)rq;
   (void)claims;
   (void)generation;
   (void)digest;
   return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
}

int server_mgmt_checkpoint_client_start(const server_http_management_config_t *config)
{
   (void)config;
   return 0;
}

void server_mgmt_checkpoint_client_stop(void)
{
}

/* The route-only fixture links server_dev_submit without the autonomy driver;
 * keep its shared intake cap at the production default. */
double wfe_autonomy_default_max_cost_usd(void)
{
   return 5.0;
}

/* Narrow response-writer seams not otherwise needed by this route-only unit. */
const char *ingress_preinject_turn_id(void)
{
   return "";
}
int anthropic_http_response_retry_after(void)
{
   return 0;
}
int server_conn_io_write_all(int fd, const void *buf, int n)
{
   const unsigned char *cursor = buf;
   int sent = 0;
   while (sent < n)
   {
      ssize_t rc = write(fd, cursor + sent, (size_t)(n - sent));
      if (rc <= 0)
         return -1;
      sent += (int)rc;
   }
   return 0;
}

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

/* Stub readiness providers. These stand in for the real snapshot provider so
 * the route's own contract can be tested without a dependency closure:
 *   _failing  a sampled snapshot with one dependency down (503)
 *   _ok       everything sampled and healthy (200)
 *   _bogus    a misbehaving provider: writes nothing, returns a nonsense
 *             status. The route must fail closed rather than pass it through. */
static int stub_ready_failing(char *resp, int cap)
{
   snprintf(resp, (size_t)cap,
            "{\"ready\":false,\"status\":\"degraded\",\"service\":\"aimee-server\","
            "\"dependencies\":{\"db1\":\"ok\",\"kb\":\"fail\"}}");
   return 503;
}

static int stub_ready_ok(char *resp, int cap)
{
   snprintf(resp, (size_t)cap,
            "{\"ready\":true,\"status\":\"ok\",\"service\":\"aimee-server\","
            "\"dependencies\":{\"db1\":\"ok\",\"kb\":\"ok\"}}");
   return 200;
}

static int stub_ready_bogus(char *resp, int cap)
{
   (void)resp;
   (void)cap;
   return 0;
}

/* Providers whose status and body disagree. The route must not pass either
 * through: a provider samples, it does not get to define the contract. */
static int stub_ready_200_not_ready(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"ready\":false,\"status\":\"degraded\",\"dependencies\":{}}");
   return 200;
}

static int stub_ready_503_ready(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"ready\":true,\"status\":\"ok\",\"dependencies\":{}}");
   return 503;
}

static int stub_ready_odd_status(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"ready\":true,\"status\":\"ok\",\"dependencies\":{}}");
   return 418;
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
static atomic_int g_op_context_clean;
static server_ctx_t g_test_server_ctx;
static int g_test_server_ctx_available = 1;

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
   if (strcmp(g_disp_method, "test.poison_op_context") == 0)
   {
      run_cmd_set_cwd("/client-only/checkout");
      agent_set_request_session("stale-session");
      agent_set_request_codex_creds("stale-token", "stale-account");
      agent_set_request_vault_principal("stale-principal");
   }
   else if (strcmp(g_disp_method, "test.inspect_op_context") == 0)
   {
      agent_request_creds_t creds;
      agent_request_creds_snapshot(&creds);
      atomic_store(&g_op_context_clean, run_cmd_get_cwd() == NULL && creds.session_id[0] == '\0' &&
                                            creds.codex_token[0] == '\0' &&
                                            creds.codex_account_id[0] == '\0' &&
                                            creds.vault_principal[0] == '\0');
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
   return g_test_server_ctx_available ? &g_test_server_ctx : NULL;
}

static void submit_and_wait_op(const char *method)
{
   char response[8192];
   assert(server_http_submit_op_run(method, "{}", CAPS_ALL, response, sizeof(response)) == 200);
   cJSON *queued = cJSON_Parse(response);
   assert(queued);
   const char *run_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(queued, "id"));
   assert(run_id && run_id[0]);
   char saved_id[128];
   snprintf(saved_id, sizeof(saved_id), "%s", run_id);
   cJSON_Delete(queued);
   openai_run_status_t status = OPENAI_RUN_QUEUED;
   for (int i = 0; i < 100; i++)
   {
      assert(openai_runs_store_status(saved_id, &status));
      if (openai_run_status_terminal(status))
         break;
      usleep(10000);
   }
   assert(status == OPENAI_RUN_COMPLETED);
}

/* The bus is not started in a unit test, so the module surface is stubbed to
 * report exactly that. obs_bus_module_call must still resolve for the link even
 * though an unattached module is answered before it is reached. */
int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return 0;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)event_kind;
   (void)stage_id;
   (void)trace_id;
   (void)deadline_ns;
   (void)request_body;
   (void)request_len;
   (void)response_body;
   (void)response_capacity;
   (void)response_len;
   (void)cancelled;
   (void)cancel_context;
   return AIMEE_MODULE_CALL_TRANSPORT;
}

/* The private workflow proxy is gone: the control plane is reached over the
 * event bus. Its round-trip test went with the transport it exercised -- the
 * request shaping it covered is now tested in Go against the same mux
 * (server-go/modules/workflows/control_test.go).
 *
 * What is still C's to answer is the unattached case. A control call with no
 * module serving the stage must say so, not hang or claim success. */
static void test_workflow_control_reports_an_unattached_module(void)
{
   char response[256];
   const char *body = "{\"proposal_md\":\"test\"}";
   int status = workflow_control_request("POST", "/v1/dev/submit", "source=release-test", body,
                                         (int)strlen(body), "webuser:release-test", 1, response,
                                         sizeof(response));
   assert(status == 503);
   assert(strstr(response, "not attached to the event bus") != NULL);
}

/* `aimee roles show` has to answer the question a log line was answering: what
 * did this role actually come to?
 *
 * A permission nothing enforces and a tool the set withholds are both invisible
 * in the frontmatter an operator wrote, and both change what the delegate can
 * do. The route carries them structurally so the CLI can print them and the
 * Personas tab can render them.
 *
 * WHAT the resolved set is belongs to the delegates module and is proved there.
 * What is proved HERE is that the route asks and reports faithfully, including
 * the case where resolution fails: a role that holds nothing is not the same as
 * a role that grants nothing, and the response says which. */
static void test_role_template_show_reports_what_the_role_came_to(void)
{
   delegate_permissions_stub_install();
   assert(role_template_write("gatekeeper",
                              "---\npermissions:\n  - tools\n  - name: deploy\n"
                              "    enforced_at: deploy-gate\n---\n\nYou gate deploys.\n") == 0);

   /* The route writes the JSON body and returns the status; the envelope is the
      caller's job. */
   char resp[8192];
   assert(route_role_template_show("gatekeeper", resp, (int)sizeof(resp)) == 200);
   cJSON *o = cJSON_Parse(resp);
   assert(o != NULL);

   cJSON *perms = cJSON_GetObjectItemCaseSensitive(o, "permissions");
   assert(cJSON_IsObject(perms));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(perms, "resolved")));

   /* Held, with the point each is bound to. */
   cJSON *held = cJSON_GetObjectItemCaseSensitive(perms, "held");
   assert(cJSON_GetArraySize(held) == 2);
   int saw_deploy_gate = 0;
   cJSON *g = NULL;
   cJSON_ArrayForEach(g, held)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(g, "name"));
      const char *at = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(g, "enforced_at"));
      if (name && strcmp(name, "deploy") == 0 && at && strcmp(at, "deploy-gate") == 0)
         saw_deploy_gate = 1;
   }
   assert(saw_deploy_gate);

   /* This role holds no shell and no repo_write, so those tools are withheld
      whatever toolset it runs with -- the thing an operator cannot see. */
   cJSON *denied = cJSON_GetObjectItemCaseSensitive(perms, "denied_tools");
   assert(cJSON_GetArraySize(denied) > 0);
   int saw_bash = 0, saw_write = 0;
   cJSON *d = NULL;
   cJSON_ArrayForEach(d, denied)
   {
      const char *tool = cJSON_GetStringValue(d);
      saw_bash |= (tool && strcmp(tool, "bash") == 0);
      saw_write |= (tool && strcmp(tool, "write_file") == 0);
   }
   assert(saw_bash && saw_write);

   cJSON_Delete(o);
   (void)role_template_delete("gatekeeper");
   printf("  PASS: test_role_template_show_reports_what_the_role_came_to\n");
}

int main(void)
{
   test_role_template_show_reports_what_the_role_came_to();
   printf("server_http: ");

   test_workflow_control_reports_an_unattached_module();

   char home[PATH_MAX];
   snprintf(home, sizeof(home), "%s/aimee-shttp-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("AIMEE_HOME", home);
   assert(compute_pool_init(&g_test_server_ctx.orchestration_pool, 4) == 0);
   g_test_server_ctx.orchestration_pool_initialized = 1;

   char resp[8192];

   /* --- GET /v1/health is a liveness probe --- */
   {
      int st = server_http_route("GET", "/v1/health", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"status\":\"ok\""));
      assert(strstr(resp, "\"service\":\"aimee-server\""));
   }

   /* --- /v1/health stays LIVENESS: unconditionally 200, never dependency-aware.
    * Readiness lives at /v1/ready. Pinning the exact body here keeps the
    * liveness contract — and the aimee-kb probe symmetry it mirrors — from
    * drifting into a readiness answer, which would make an orchestrator restart
    * a healthy process during a transient dependency outage. --- */
   {
      int st = server_http_route("GET", "/v1/health", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strcmp(resp, "{\"status\":\"ok\",\"service\":\"aimee-server\"}") == 0);
   }

   /* --- GET /v1/ready is readiness, and fails closed ---
    * Unregistered means "not sampled yet", which must never read as ready. The
    * body keeps one shape in every case so a client parsing .ready/.dependencies
    * never special-cases an unsampled server. */
   {
      /* No provider: unknown ⇒ 503, not 200 and not a bare error shape. */
      int st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(strstr(resp, "\"status\":\"unknown\""));
      assert(strstr(resp, "\"dependencies\":"));

      /* A sampled snapshot with a dependency down reports 503 and names it.
       * This is the assertion that would fail against a blind endpoint: swap in
       * stub_ready_ok below and it breaks, which is what proves the test
       * detects blindness rather than observing a constant. */
      server_http_set_ready_provider(stub_ready_failing);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(strstr(resp, "\"kb\":\"fail\""));

      /* Everything healthy ⇒ 200. */
      server_http_set_ready_provider(stub_ready_ok);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"ready\":true"));

      /* A misbehaving provider must not be able to advertise readiness. */
      server_http_set_ready_provider(stub_ready_bogus);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));

      /* Status and body must agree. A 200 that does not say ready:true, a 503
       * that does, and any status outside {200,503} are all provider bugs — the
       * route replaces them with the fail-closed answer rather than forwarding a
       * contradiction a caller would have to reconcile. */
      server_http_set_ready_provider(stub_ready_200_not_ready);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));

      server_http_set_ready_provider(stub_ready_503_ready);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(!strstr(resp, "\"ready\":true"));

      server_http_set_ready_provider(stub_ready_odd_status);
      st = server_http_route("GET", "/v1/ready", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));

      server_http_set_ready_provider(NULL);
   }

   /* Legacy management environment cannot bypass the composed action packet. */
   {
      platform_setenv("AIMEE_MGMT_JWKS", "{\"keys\":[]}");
      platform_setenv("AIMEE_MGMT_ISSUER", "legacy-issuer");
      platform_setenv("AIMEE_MGMT_AUDIENCE", "legacy-audience");
      platform_setenv("AIMEE_MGMT_PEER_CN", "legacy-peer");
      int st = server_http_route("POST", "/v1/management/action",
                                 "{\"token\":\"legacy\",\"target\":\"x\"}", 31, resp, sizeof(resp));
      assert(st == 403);
      assert(!strcmp(resp, "{\"result\":\"denied\",\"effect\":\"none\"}"));
   }

   /* Shutdown closes the full-action gate before dependency teardown. An
    * already-admitted request remains accounted for until its final response. */
   {
      server_http_management_actions_start();
      assert(server_http_management_action_begin() == 0);
      server_http_management_actions_shutdown_begin();
      assert(!server_http_management_action_allowed());
      assert(server_http_management_action_begin() != 0);
      server_http_management_action_end();
      server_http_management_actions_stop_and_wait();
      server_http_management_actions_start();
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
      st = server_http_route("GET", "/v1/runs/oprun_gprior_1700000000_1", NULL, 0, resp,
                             sizeof(resp));
      assert(st == 410);
      assert(strstr(resp, "\"status\":\"interrupted\""));
      char evicted_id[160];
      snprintf(evicted_id, sizeof(evicted_id), "/v1/runs/oprun_%s_1700000000_1",
               openai_runs_store_generation());
      st = server_http_route("GET", evicted_id, NULL, 0, resp, sizeof(resp));
      assert(st == 410);
      assert(strstr(resp, "\"status\":\"evicted\""));
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

   /* Go owns workflow state, but the public C resource plane must forward to it.
    * A control plane that is not serving the stage is a service outage, not a
    * retired 410 endpoint. The stub above reports the module as unattached. */
   {
      int st = server_http_route("POST", "/v1/dev/submit", "{}", 2, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "not attached to the event bus"));
      st = server_http_route("GET", "/v1/workflow/items/wi_test", NULL, 0, resp, sizeof(resp));
      assert(st == 503);
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
      st = server_http_route("GET", "/v1/kb/status", "{\"project\":\"release-e2e\"}", 25, resp,
                             sizeof(resp));
      assert(st == 200 && strstr(resp, "release-e2e"));
      server_http_set_kb_status_provider(NULL);
      st = server_http_route("GET", "/v1/kb/ingest/status", NULL, 0, resp, sizeof(resp));
      assert(st == 200 && strstr(resp, "\"pending\":0"));
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

   /* --- server_http_authorize_multi: pairing a client must not evict one ---
    *
    * Enrolling a client used to be implemented AS rotating the single global
    * bearer, so the second client to pair silently invalidated the first and
    * every already-paired client started failing at the same instant. The whole
    * point of the extra set is that both credentials keep working. */
   {
      const char *primary = "primary-token-aaaaaaaaaaaaaaaaaaaaaaaa";
      const char *e1 = "enrolled-one-bbbbbbbbbbbbbbbbbbbbbbbbbb";
      const char *e2 = "enrolled-two-cccccccccccccccccccccccccc";
      const char *extra[] = {e1, e2};

      char hdr[384];
      snprintf(hdr, sizeof(hdr), "Bearer %s", primary);
      assert(server_http_authorize_multi(1, primary, extra, 2, hdr, NULL, 0) == 0);

      /* ...and BOTH enrolled clients still work — the property that was missing */
      snprintf(hdr, sizeof(hdr), "Bearer %s", e1);
      assert(server_http_authorize_multi(1, primary, extra, 2, hdr, NULL, 0) == 0);
      snprintf(hdr, sizeof(hdr), "Bearer %s", e2);
      assert(server_http_authorize_multi(1, primary, extra, 2, hdr, NULL, 0) == 0);

      /* An unrelated token is still refused: the set is additive, not permissive. */
      assert(server_http_authorize_multi(1, primary, extra, 2, "Bearer nope", NULL, 0) == 401);
      assert(server_http_authorize_multi(1, primary, extra, 2, NULL, NULL, 0) == 401);

      /* x-api-key honours the extra set exactly as Authorization does. */
      assert(server_http_authorize_multi(1, primary, extra, 2, NULL, e2, 0) == 0);
      assert(server_http_authorize_multi(1, primary, extra, 2, NULL, "nope", 0) == 401);

      /* A near-miss must not pass: no prefix/substring acceptance. */
      snprintf(hdr, sizeof(hdr), "Bearer %.*s", 10, e1);
      assert(server_http_authorize_multi(1, primary, extra, 2, hdr, NULL, 0) == 401);

      /* With no extras it must behave exactly like the single-token function. */
      assert(server_http_authorize_multi(1, primary, NULL, 0, "Bearer nope", NULL, 0) ==
             server_http_authorize(1, primary, "Bearer nope", NULL, 0));
      snprintf(hdr, sizeof(hdr), "Bearer %s", primary);
      assert(server_http_authorize_multi(1, primary, NULL, 0, hdr, NULL, 0) == 0);

      /* UDS stays unauthenticated regardless of the extra set. */
      assert(server_http_authorize_multi(0, primary, extra, 2, "Bearer nope", NULL, 0) == 0);

      /* A 503 (no bearer configured on TCP) is a server misconfiguration that
       * extra tokens must not paper over. */
      assert(server_http_authorize_multi(1, "", extra, 2, "Bearer nope", NULL, 0) == 503);

      /* Empty slots in the set are skipped, not treated as a wildcard match on
       * an empty presented token. */
      const char *sparse[] = {"", e1, ""};
      assert(server_http_authorize_multi(1, primary, sparse, 3, "Bearer ", NULL, 0) == 401);
      snprintf(hdr, sizeof(hdr), "Bearer %s", e1);
      assert(server_http_authorize_multi(1, primary, sparse, 3, hdr, NULL, 0) == 0);

      /* Live publication preserves enrolled clients at startup, while explicit
       * rotation revokes the entire old set atomically. */
      char live[256] = "";
      server_http_set_bearer_extra(extra, 2);
      server_http_update_primary_bearer(live, sizeof(live), primary, 0);
      assert(server_http_enrolled_bearer_count() == 2);
      snprintf(hdr, sizeof(hdr), "Bearer %s", e1);
      assert(server_http_authorize_enrolled(1, live, hdr, NULL, 0) == 0);
      server_http_update_primary_bearer(live, sizeof(live), "rotated-primary", 1);
      assert(server_http_enrolled_bearer_count() == 0);
      assert(server_http_authorize_enrolled(1, live, hdr, NULL, 0) == 401);
      assert(server_http_authorize_enrolled(1, live, "Bearer rotated-primary", NULL, 0) == 0);

      /* bearer_tokens_extra predates wizard enrollment and permits existing
       * operator-supplied values longer than the new 64-hex minted token. An
       * upgrade must not truncate and silently revoke those clients. */
      char long_token[192];
      memset(long_token, 'L', sizeof(long_token) - 1);
      long_token[sizeof(long_token) - 1] = '\0';
      const char *long_extra[] = {long_token};
      server_http_set_bearer_extra(long_extra, 1);
      snprintf(hdr, sizeof(hdr), "Bearer %s", long_token);
      assert(server_http_authorize_enrolled(1, live, hdr, NULL, 0) == 0);
      server_http_set_bearer_extra(NULL, 0);

      printf("  PASS: authorize_multi accepts every enrolled client, rejects the rest\n");
   }

   /* --- server_http_auth_error_body: the 401 must carry a way out ---
    *
    * A bearer rotation invalidates every already-paired client at once, and the
    * old body said only "missing or invalid bearer token" — indistinguishable
    * from a typo, with no recovery path. Recovery must use the trusted local
    * socket; the message must never direct an operator to a plaintext config
    * credential because API bearers are Vault-only. */
   {
      const char *b401 = server_http_auth_error_body(401);
      assert(b401 != NULL);
      /* still identifies the failure */
      assert(strstr(b401, "missing or invalid bearer token") != NULL);
      assert(strstr(b401, "\"type\":\"authentication_error\"") != NULL);
      /* ...and now says how to recover through the kernel-attested local path. */
      assert(strstr(b401, "rotation") != NULL);
      assert(strstr(b401, "aimee api enable") != NULL);
      assert(strstr(b401, "aimee remote set") != NULL);
      assert(strstr(b401, "Vault-only") != NULL);
      assert(strstr(b401, "aimee.api.bearer_token") == NULL);

      /* The 503 case is a server misconfiguration, not a client credential
       * problem — it must NOT tell the caller to go re-pair. */
      const char *b503 = server_http_auth_error_body(503);
      assert(b503 != NULL);
      assert(strstr(b503, "requires a configured bearer token") != NULL);
      assert(strstr(b503, "aimee remote set") == NULL);
      assert(strcmp(b401, b503) != 0);

      /* Both must be parseable JSON objects, since clients decode before display. */
      cJSON *j401 = cJSON_Parse(b401);
      assert(j401 != NULL);
      cJSON_Delete(j401);
      cJSON *j503 = cJSON_Parse(b503);
      assert(j503 != NULL);
      cJSON_Delete(j503);
      printf("  PASS: auth_error_body carries rotation recovery path\n");
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

   /* --- API primaries are operator/random Vault credentials, never a published
    *     value with hidden special capabilities. Enrollment remains additive. --- */
   {
      const char *PRIMARY = "unit-test-random-primary";
      const char *enrolled[] = {"wizard-user-token"};
      server_http_set_bearer_extra(enrolled, 1);
      int bootstrap_only = -1;
      assert(server_http_authorize_enrolled_request(1, PRIMARY, "Bearer unit-test-random-primary",
                                                    NULL, 0, &bootstrap_only) == 0);
      assert(bootstrap_only == 0);
      assert(server_http_authorize_enrolled_request(1, PRIMARY, "Bearer wizard-user-token", NULL, 0,
                                                    &bootstrap_only) == 0);
      assert(bootstrap_only == 0);
      server_http_set_bearer_extra(NULL, 0);

      uint32_t primary_caps =
          server_http_effective_conn_caps(1, PRIMARY, SERVER_REMOTE_WRITES_OFF, 1, 0);
      assert((primary_caps & CAP_SESSION_ADMIN) == 0);
      assert((primary_caps & CAP_DELEGATE) == 0);
      assert(server_http_route_allowed_caps(1, CAPS_AUTHENTICATED, "POST", "/v1/api/enroll_bearer",
                                            SERVER_REMOTE_WRITES_OFF) == 1);
      assert(server_http_route_allowed_caps(1, CAPS_READ_ONLY | CAP_DELEGATE, "POST",
                                            "/v1/cert/sign", SERVER_REMOTE_WRITES_OFF) == 1);
      assert((server_http_enrollment_caps(CAPS_READ_ONLY, 1, 0, 1, "deployment-token", "POST",
                                          "/v1/cert/sign") &
              CAP_DELEGATE) != 0);
      assert(server_http_enrollment_caps(CAPS_READ_ONLY, 1, 0, 0, "deployment-token", "POST",
                                         "/v1/cert/sign") == CAPS_READ_ONLY);
      assert(server_http_enrollment_caps(CAPS_READ_ONLY, 1, 0, 1, "scope:read", "POST",
                                         "/v1/cert/sign") == CAPS_READ_ONLY);
      assert((server_http_enrollment_caps(CAPS_READ_ONLY, 1, 0, 1, "deployment-token", "POST",
                                          "/v1/api/enroll_bearer") &
              CAP_SESSION_ADMIN) != 0);
   }

   /* --- P5-B3b dedicated management transport classification. The two
    *     nonce/status routes have no generic cert/bearer fallback, while a
    *     management-profile leaf cannot escape onto any other route. --- */
   {
      assert(server_http_management_auth("POST", "/v1/management/challenge", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("GET", "/v1/management/health", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("GET", "/v1/management/health", 1, 0, 0, NULL) ==
             SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("GET", "/v1/management/health", 1, 1, 0,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("GET", "/v1/management/health", 1, 1, 1,
                                         "generic-client") == SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("GET", "/v1/health", 1, 1, 1, "p5-kb-management") ==
             SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("POST", "/v1/management/action", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("POST", "/v1/management/action/challenge", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("POST", "/v1/management/read/challenge", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("GET", "/v1/management/read/agents", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("POST", "/v1/management/read/config/challenge", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("GET", "/v1/management/read/config", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_ALLOW);
      assert(server_http_management_auth("GET", "/v1/health", 0, 1, 0, "generic-client") ==
             SERVER_HTTP_MANAGEMENT_NOT_APPLICABLE);
      assert(server_http_management_auth("POST", "/v1/management/health", 1, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_DENY);
      /* Cross-lane denial is unconditional: neither the exact management leaf
       * on data TLS nor a bearer/UDS request can reach these handlers. */
      assert(server_http_management_auth("POST", "/v1/management/challenge", 0, 1, 1,
                                         "p5-kb-management") == SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("GET", "/v1/management/health", 0, 0, 0, NULL) ==
             SERVER_HTTP_MANAGEMENT_DENY);
      assert(server_http_management_auth("GET", "/v1/health", 0, 1, 1, "p5-kb-management") ==
             SERVER_HTTP_MANAGEMENT_DENY);
   }

   /* --- Dedicated management environment packet and bind policy. --- */
   {
      static const char *const vars[] = {
          "AIMEE_SERVER_MGMT_BIND",
          "AIMEE_SERVER_MGMT_PORT",
          "AIMEE_SERVER_MGMT_TLS_CERT",
          "AIMEE_SERVER_MGMT_TLS_KEY", /* forbidden legacy path input */
          "AIMEE_SERVER_MGMT_CLIENT_CA",
          "AIMEE_SERVER_ID",
          "AIMEE_MGMT_STATUS_KEY_ID",
          "AIMEE_MGMT_STATUS_PUBLIC_KEY",
          "AIMEE_SERVER_MGMT_STATUS_ENDPOINT",
          "AIMEE_SERVER_MGMT_STATUS_CA_FILE",
          "AIMEE_SERVER_MGMT_STATUS_LEAF_PIN",
          "AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN",
          "AIMEE_SERVER_MGMT_STATUS_CLIENT_CERT",
          "AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY", /* forbidden legacy path input */
          "AIMEE_SERVER_MGMT_ISSUER",
          "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE",
      };
      for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
         unsetenv(vars[i]);
      runtime_secret_remove("AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY");
      runtime_secret_remove("AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY");
      server_http_management_config_t mc;
      assert(server_http_management_config_from_env(&mc) == 0 && !mc.enabled);
      uint32_t bind_addr = 0;
      assert(server_http_management_bind_addr("127.0.0.1", &bind_addr) == 0);
      assert(bind_addr == htonl(INADDR_LOOPBACK));
      assert(server_http_management_bind_addr("0.0.0.0", &bind_addr) == -1);
      assert(server_http_management_bind_addr("255.255.255.255", &bind_addr) == -1);
      assert(server_http_management_bind_addr("169.254.1.1", &bind_addr) == -1);
      assert(server_http_management_bind_addr("224.0.0.1", &bind_addr) == -1);
      assert(server_http_management_bind_addr("127.000.0.1", &bind_addr) == -1);
      assert(server_http_management_bind_addr("localhost", &bind_addr) == -1);
      assert(server_http_management_bind_addr("::1", &bind_addr) == -1);
      assert(server_http_management_bind_addr(NULL, &bind_addr) == -1);
      assert(server_http_management_bind_addr("127.0.0.1", NULL) == -1);

      setenv("AIMEE_SERVER_MGMT_BIND", "127.0.0.1", 1);
      assert(server_http_management_config_from_env(&mc) == -1); /* partial */
      setenv("AIMEE_SERVER_MGMT_PORT", "9443", 1);
      setenv("AIMEE_SERVER_MGMT_TLS_CERT", "/etc/aimee/management/server.pem", 1);
      setenv("AIMEE_SERVER_MGMT_CLIENT_CA", "/etc/aimee/management/client-ca.pem", 1);
      assert(runtime_secret_store("AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY", "server-key-pem") == 0);
      setenv("AIMEE_SERVER_ID", "p5b3c-server", 1);
      setenv("AIMEE_MGMT_STATUS_KEY_ID", "status-v1", 1);
      setenv("AIMEE_MGMT_STATUS_PUBLIC_KEY",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
      setenv("AIMEE_SERVER_MGMT_STATUS_ENDPOINT", "https://kb.test", 1);
      setenv("AIMEE_SERVER_MGMT_STATUS_CA_FILE", "/etc/aimee/management/kb-ca.pem", 1);
      setenv("AIMEE_SERVER_MGMT_STATUS_LEAF_PIN",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
      setenv("AIMEE_SERVER_MGMT_STATUS_CLIENT_CERT", "/etc/aimee/management/client.pem", 1);
      assert(runtime_secret_store("AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY",
                                  "status-client-key-pem") == 0);
      setenv("AIMEE_SERVER_MGMT_ISSUER", "https://kb.test", 1);
      setenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE", "/etc/aimee/management/jwks-roots.pem", 1);
      assert(server_http_management_config_from_env(&mc) == 0 && mc.enabled && mc.port == 9443);
      assert(strcmp(mc.bind, "127.0.0.1") == 0);
      assert(strcmp(mc.cert, "/etc/aimee/management/server.pem") == 0);
      assert(strcmp(mc.status_endpoint, "https://kb.test") == 0);
      unsetenv("AIMEE_SERVER_MGMT_BIND");
      assert(server_http_management_config_from_env(&mc) == 0 && mc.enabled);
      assert(strcmp(mc.bind, "127.0.0.1") == 0);
      setenv("AIMEE_SERVER_MGMT_BIND", "127.0.0.1", 1);

      const char *bad_ports[] = {"", "0", "09443", "+9443", "65536", "9443x"};
      for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++)
      {
         setenv("AIMEE_SERVER_MGMT_PORT", bad_ports[i], 1);
         assert(server_http_management_config_from_env(&mc) == -1);
      }
      setenv("AIMEE_SERVER_MGMT_PORT", "9443", 1);
      setenv("AIMEE_SERVER_MGMT_TLS_KEY", "/etc/aimee/management/server.key", 1);
      assert(server_http_management_config_from_env(&mc) == -1);
      unsetenv("AIMEE_SERVER_MGMT_TLS_KEY");
      setenv("AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY", "/etc/aimee/management/client.key", 1);
      assert(server_http_management_config_from_env(&mc) == -1);
      unsetenv("AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY");
      setenv("AIMEE_SERVER_MGMT_STATUS_ENDPOINT", "https://kb.test/v1/management/status", 1);
      assert(server_http_management_config_from_env(&mc) == -1);
      setenv("AIMEE_SERVER_MGMT_STATUS_ENDPOINT", "https://kb.test", 1);
      setenv("AIMEE_SERVER_ID", "bad/server", 1);
      assert(server_http_management_config_from_env(&mc) == -1);
      setenv("AIMEE_SERVER_ID", "p5b3c-server", 1);
      setenv("AIMEE_MGMT_STATUS_PUBLIC_KEY",
             "A123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
      assert(server_http_management_config_from_env(&mc) == -1);
      for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
         unsetenv(vars[i]);
      runtime_secret_remove("AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY");
      runtime_secret_remove("AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY");
   }

   /* --- Management requests use one exact, bodyless HTTP/1.1 frame. --- */
   {
      const char challenge[] = "POST /v1/management/challenge HTTP/1.1\r\nHost: server.test\r\n"
                               "Content-Type: application/json\r\nContent-Length: 0\r\n"
                               "Connection: keep-alive\r\n\r\n";
      const char health[] = "GET /v1/management/health HTTP/1.1\r\nHost: server.test\r\n"
                            "X-Aimee-Management-Status: staple\r\n"
                            "Content-Type: application/json\r\nContent-Length: 0\r\n"
                            "Connection: keep-alive\r\n\r\n";
      const char action_challenge[] =
          "POST /v1/management/action/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
      const char action[] =
          "POST /v1/management/action HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 42\r\nConnection: close\r\n"
          "Authorization: Bearer token\r\nX-Aimee-Management-Status: staple\r\n\r\n";
      const char read_challenge[] =
          "POST /v1/management/read/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
      const char read_agents[] =
          "GET /v1/management/read/agents HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Length: 0\r\nConnection: close\r\nAuthorization: Bearer token\r\n"
          "X-Aimee-Management-Status: staple\r\n\r\n";
      assert(server_http_management_framing_valid("POST", "/v1/management/challenge", challenge,
                                                  strlen(challenge)) == 1);
      assert(server_http_management_framing_valid("GET", "/v1/management/health", health,
                                                  strlen(health)) == 1);
      assert(server_http_management_action_framing_valid("POST", "/v1/management/action/challenge",
                                                         action_challenge,
                                                         strlen(action_challenge)) == 1);
      assert(server_http_management_action_framing_valid("POST", "/v1/management/action", action,
                                                         strlen(action)) == 1);
      assert(server_http_management_read_framing_valid("POST", "/v1/management/read/challenge",
                                                       read_challenge,
                                                       strlen(read_challenge)) == 1);
      assert(server_http_management_read_framing_valid("GET", "/v1/management/read/agents",
                                                       read_agents, strlen(read_agents)) == 1);
      const char read_config_challenge[] =
          "POST /v1/management/read/config/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
      const char read_config[] = "GET /v1/management/read/config HTTP/1.1\r\nHost: server.test\r\n"
                                 "Authorization: Bearer token\r\nX-Aimee-Management-Status: {}\r\n"
                                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
      assert(server_http_management_read_framing_valid(
                 "POST", "/v1/management/read/config/challenge", read_config_challenge,
                 strlen(read_config_challenge)) == 1);
      assert(server_http_management_read_framing_valid("GET", "/v1/management/read/config",
                                                       read_config, strlen(read_config)) == 1);
      char read_with_type[1024];
      snprintf(read_with_type, sizeof(read_with_type), "%.*sContent-Type: application/json\r\n\r\n",
               (int)(strlen(read_agents) - 2), read_agents);
      assert(!server_http_management_read_framing_valid("GET", "/v1/management/read/agents",
                                                        read_with_type, strlen(read_with_type)));
      const char read_empty_length[] =
          "POST /v1/management/read/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: \r\n"
          "Connection: keep-alive\r\n\r\n";
      const char read_bad_length[] =
          "POST /v1/management/read/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: +0\r\n"
          "Connection: keep-alive\r\n\r\n";
      const char read_overflow_length[] =
          "POST /v1/management/read/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 184467440737095516160\r\n"
          "Connection: keep-alive\r\n\r\n";
      assert(!server_http_management_read_framing_valid(
          "POST", "/v1/management/read/challenge", read_empty_length, strlen(read_empty_length)));
      assert(!server_http_management_read_framing_valid("POST", "/v1/management/read/challenge",
                                                        read_bad_length, strlen(read_bad_length)));
      assert(!server_http_management_read_framing_valid("POST", "/v1/management/read/challenge",
                                                        read_overflow_length,
                                                        strlen(read_overflow_length)));
      const char closing_challenge[] =
          "POST /v1/management/action/challenge HTTP/1.1\r\nHost: server.test\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      assert(!server_http_management_action_framing_valid(
          "POST", "/v1/management/action/challenge", closing_challenge, strlen(closing_challenge)));
      char duplicate_auth[1024];
      snprintf(duplicate_auth, sizeof(duplicate_auth), "%.*sAuthorization: Bearer second\r\n\r\n",
               (int)(strlen(action) - 2), action);
      assert(!server_http_management_action_framing_valid("POST", "/v1/management/action",
                                                          duplicate_auth, strlen(duplicate_auth)));
      const char duplicate[] = "GET /v1/management/health HTTP/1.1\r\nContent-Length: 0\r\n"
                               "Content-Length: 0\r\n\r\n";
      const char duplicate_status[] =
          "GET /v1/management/health HTTP/1.1\r\nHost: x\r\n"
          "X-Aimee-Management-Status: one\r\nX-Aimee-Management-Status: two\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
      const char transfer[] = "GET /v1/management/health HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                              "Content-Length: 0\r\n\r\n";
      const char expect[] = "GET /v1/management/health HTTP/1.1\r\nExpect: 100-continue\r\n"
                            "Content-Length: 0\r\n\r\n";
      const char upgrade[] = "GET /v1/management/health HTTP/1.1\r\nUpgrade: websocket\r\n"
                             "Content-Length: 0\r\n\r\n";
      const char connection_upgrade[] =
          "GET /v1/management/health HTTP/1.1\r\nHost: x\r\n"
          "X-Aimee-Management-Status: staple\r\nContent-Type: application/json\r\n"
          "Content-Length: 0\r\nConnection: Upgrade\r\n\r\n";
      const char body[] = "POST /v1/management/challenge HTTP/1.1\r\nContent-Length: 1\r\n\r\nx";
      const char no_length[] = "GET /v1/management/health HTTP/1.1\r\nHost: x\r\n\r\n";
      const char query[] = "GET /v1/management/health?x=1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
      const char lf[] = "GET /v1/management/health HTTP/1.1\nContent-Length: 0\n\n";
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", duplicate,
                                                   strlen(duplicate)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", duplicate_status,
                                                   strlen(duplicate_status)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", transfer,
                                                   strlen(transfer)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", expect,
                                                   strlen(expect)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", upgrade,
                                                   strlen(upgrade)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health",
                                                   connection_upgrade, strlen(connection_upgrade)));
      assert(!server_http_management_framing_valid("POST", "/v1/management/challenge", body,
                                                   strlen(body)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", no_length,
                                                   strlen(no_length)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", query,
                                                   strlen(query)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", lf, strlen(lf)));
      assert(!server_http_management_framing_valid("GET", "/v1/management/health", health,
                                                   strlen(health) - 1));
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
      assert(server_http_route_caps("GET", "/v1/ready") == 0);
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
      /* Help is a narrow read endpoint. Keep generic MCP execution privileged:
       * the help handler force-selects get_help before dispatch. */
      assert(server_http_route_caps("POST", "/v1/help") == CAP_SESSION_READ);
      assert(server_http_route_caps("POST", "/v1/help") ==
             server_capability_for_method("help.get"));
      assert(server_http_route_caps("POST", "/v1/mcp/call") == CAP_TOOL_EXECUTE);

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

      /* A `mirror` registration is seeded by fetching the client's head from its
       * remote, so BOTH must survive the REST hop. This route forwarded only
       * --provider, so a mirror registration over REST was refused for a missing
       * --remote — and the reverse channel, which registers this way, had no
       * route to the sandboxed tier at all. */
      {
         const char *args[WS_ADD_FLAG_ARGS_MAX];
         int n = workspace_add_flag_args("mirror", "git@github.com:o/r.git", "abc123", args,
                                         WS_ADD_FLAG_ARGS_MAX);
         assert(n == 6);
         assert(strcmp(args[0], "--provider") == 0 && strcmp(args[1], "mirror") == 0);
         assert(strcmp(args[2], "--remote") == 0 && strcmp(args[3], "git@github.com:o/r.git") == 0);
         assert(strcmp(args[4], "--head") == 0 && strcmp(args[5], "abc123") == 0);

         /* Absent coordinates stay absent — `detached`/`shared` take neither, and
          * an empty --remote would be a worse error than no flag. */
         n = workspace_add_flag_args("detached", "", "", args, WS_ADD_FLAG_ARGS_MAX);
         assert(n == 2);
         assert(strcmp(args[0], "--provider") == 0 && strcmp(args[1], "detached") == 0);

         /* No provider, nothing to say. */
         assert(workspace_add_flag_args("", "r", "h", args, WS_ADD_FLAG_ARGS_MAX) == 0);
      }
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

      /* INDEXING IS A DATA-PLANE WRITE. It was absent from g_v1_write_ops, so the
       * write-tier gate never saw it: on a clean install the same bearer got 403
       * on /v1/memory/store and 200 on /v1/index/ingest, and kb queued curator
       * work for the new project. Registration staying exempt while its ingest is
       * gated is exactly what §1.4 and Parts 2-4 of QUICKSTART promise, so the
       * pair is asserted together — they are easy to conflate, and conflating
       * them is how the gap got in. */
      assert(server_http_route_is_local_only("POST", "/v1/index/ingest") == 1);
      /* The index READ family must stay ungated, or every query needs a grant. */
      assert(server_http_route_is_local_only("POST", "/v1/index/find") == 0);
      assert(server_http_route_is_local_only("POST", "/v1/index/deps") == 0);
      assert(server_http_route_is_local_only("POST", "/v1/memory/search") == 0);

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
      const char *exec_paths[] = {"/v1/delegate/launch",   "/v1/delegate/backend_exec",
                                  "/v1/roundtable/review", "/v1/cron/add",
                                  "/v1/model/add",         "/v1/worktree/gc",
                                  "/v1/catalog/refresh",   "/v1/api/disable"};
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
      /* The mirror tier's client-diff upload is the workspace resource plane,
       * driven by a remote fs authority exactly like workspace.add and the
       * runner channel, so it is TCP-exempt. It needs tool:execute, so WITHOUT
       * the exemption the gate would demand remote_writes=full: at the default a
       * thin client could never upload, and the server would reconstruct a clean
       * checkout at head with the client's uncommitted work silently missing.
       *
       * The cap is derived from the row's op (the row passes 0, and
       * v1_route_caps_lookup prefers the op), which is why it reads as
       * tool:execute here despite the literal 0 in the table. */
      assert(server_http_route_caps("POST", "/v1/workspace/mirror-sync") == CAP_TOOL_EXECUTE);
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/workspace/mirror-sync",
                                       SERVER_REMOTE_WRITES_OFF) == 1);
      /* The sibling it shares a plane with, for contrast. */
      assert(server_http_route_allowed(1, "plain", "POST", "/v1/workspaces",
                                       SERVER_REMOTE_WRITES_OFF) == 1);

      assert(server_http_route_caps("POST", "/v1/roundtable/review") == CAP_DELEGATE);
      assert(server_http_route_allowed(1, "scope:project:alpha:s3cr3t", "POST",
                                       "/v1/roundtable/review", SERVER_REMOTE_WRITES_FULL) == 0);
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
      /* MCP startup is catalog introspection, not tool execution. Query-only
       * remote clients must be able to complete tools/list; mcp.call remains
       * separately gated by CAP_TOOL_EXECUTE. */
      assert(server_capability_for_method("mcp.tools_list") == CAP_SESSION_READ);
      assert(server_http_route_caps("GET", "/v1/mcp/tools_list") == CAP_SESSION_READ);
      assert(server_http_route_allowed_caps(1, CAPS_READ_ONLY, "GET", "/v1/mcp/tools_list",
                                            SERVER_REMOTE_WRITES_OFF) == 1);
      assert(server_capability_for_method("mcp.call") == CAP_TOOL_EXECUTE);
      assert(server_http_route_allowed_caps(1, CAPS_READ_ONLY, "POST", "/v1/mcp/call",
                                            SERVER_REMOTE_WRITES_OFF) == 0);
      assert(server_http_route_caps("DELETE", "/v1/role_templates/qa") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("GET", "/v1/roundtables") == CAP_SESSION_READ);
      assert(server_http_route_caps("PUT", "/v1/roundtables/default") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("DELETE", "/v1/roundtables/default") == CAP_SESSION_ADMIN);
      assert(server_http_route_caps("POST", "/v1/roundtables/active") == CAP_SESSION_ADMIN);
      /* With no bootstrap record at all, the gate keeps its historical shape. */
      assert(route_roundtable_mutation_authorized("webuser:admin") == 1);
      assert(route_roundtable_mutation_authorized("webuser:") == 0);
      assert(route_roundtable_mutation_authorized("webuser:alice") == 0);
      assert(route_roundtable_mutation_authorized("uid:1000") == 0);
      assert(route_roundtable_mutation_authorized("cert:operator") == 0);
      assert(route_roundtable_mutation_authorized(NULL) == 0);

      /* Once setup replaces the generated bootstrap login, THAT account is the
       * appliance administrator. Hardcoding "admin" locked the real operator out
       * of every roundtable policy mutation on their own appliance — creating a
       * preset, and "save as default" (POST /v1/roundtables/active) — while the
       * browser only reported "administrator access required". */
      {
         char wc[512];
         snprintf(wc, sizeof(wc), "%s/webchat", config_default_dir());
         assert(mkdir(wc, 0700) == 0 || errno == EEXIST);
         char marker[600];
         snprintf(marker, sizeof(marker), "%s/bootstrap-replaced", wc);
         FILE *mf = fopen(marker, "w");
         assert(mf);
         fputs("virant\n", mf);
         fclose(mf);

         assert(route_roundtable_mutation_authorized("webuser:virant") == 1);
         /* and the pre-replacement name is no longer privileged */
         assert(route_roundtable_mutation_authorized("webuser:admin") == 0);
         assert(route_roundtable_mutation_authorized("webuser:alice") == 0);
         assert(route_roundtable_mutation_authorized("uid:0") == 0);
         unlink(marker);

         /* Before replacement, the recorded bootstrap account governs. The file
          * is "<explicit|generated>:<name>". */
         char bu[600];
         snprintf(bu, sizeof(bu), "%s/bootstrap-user", wc);
         FILE *bf = fopen(bu, "w");
         assert(bf);
         fputs("generated:aimee-0123456789ab\n", bf);
         fclose(bf);
         assert(route_roundtable_mutation_authorized("webuser:aimee-0123456789ab") == 1);
         assert(route_roundtable_mutation_authorized("webuser:admin") == 0);
         unlink(bu);
      }
      /* An unset roundtable.default does not mean "no active panel": resolution
       * falls back to the preset literally named "default", which is the one the
       * image seeds. The list used to report active:"" for every entry, so the
       * Roundtable tab showed nothing selected while reviews were in fact
       * resolving through that preset — and "save as default" appeared to do
       * nothing even when it succeeded. */
      {
         char rtdir[512];
         snprintf(rtdir, sizeof(rtdir), "%s/roundtables", config_default_dir());
         assert(mkdir(rtdir, 0700) == 0 || errno == EEXIST);
         char seeded[600];
         snprintf(seeded, sizeof(seeded), "%s/default.json", rtdir);
         FILE *sf = fopen(seeded, "w");
         assert(sf);
         fputs(
             "{\"name\":\"default\",\"seats\":[{\"model\":\"$random\",\"persona\":\"reviewer\"}]}",
             sf);
         fclose(sf);

         char list_resp[4096];
         int list_st = route_roundtables_list(list_resp, sizeof(list_resp));
         assert(list_st == 200);
         assert(strstr(list_resp, "\"active\":\"default\"") != NULL);
         unlink(seeded);
      }
      assert(roundtable_policy_config_key("roundtable.default") == 1);
      assert(roundtable_policy_config_key("roundtable.require_evidence") == 1);
      assert(roundtable_policy_config_key("autonomy.concurrency") == 0);
      assert(roundtable_policy_config_key(NULL) == 0);
      char roundtable_resp[512];
      const char *agent_attempt = "{\"seats\":[{\"model\":\"codex\"}]}";
      int roundtable_st =
          server_http_route("PUT", "/v1/roundtables/agent-attempt", agent_attempt,
                            (int)strlen(agent_attempt), roundtable_resp, sizeof(roundtable_resp));
      assert(roundtable_st == 403);
      assert(strstr(roundtable_resp, "authenticated appliance administrator") != NULL);
      const char *config_attempt = "{\"key\":\"roundtable.default\",\"value\":\"agent-choice\"}";
      roundtable_st =
          server_http_route("POST", "/v1/config/set", config_attempt, (int)strlen(config_attempt),
                            roundtable_resp, sizeof(roundtable_resp));
      assert(roundtable_st == 403);
      assert(strstr(roundtable_resp, "authenticated appliance administrator") != NULL);
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
      assert(server_http_route_caps("POST", "/v1/sessions/list") == CAP_SESSION_READ);
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

      /* CAP_GRANT_ADMIN sits inside CAPS_ALL (so the UDS operator has it) and OUTSIDE
       * CAPS_AUTHENTICATED (so a mere authenticated bearer does not), matching
       * CAP_WORKFLOW_ADMIN and CAP_SHADOW_ADMIN. A bearer able to administer grants could
       * grant ITSELF a higher tier, which is why it cannot be in the authenticated set. */
      assert((CAPS_ALL & CAP_GRANT_ADMIN) == CAP_GRANT_ADMIN);
      assert((CAPS_AUTHENTICATED & CAP_GRANT_ADMIN) == 0);
      assert((CAPS_READ_ONLY & CAP_GRANT_ADMIN) == 0);

      /* Grant administration is GONE from aimee-server.
       *
       * It used to be UDS-only here, because a TCP bearer at remote_writes=full
       * holds CAPS_ALL and could otherwise have widened its own write tier. The
       * stronger answer is that the server does not proxy it at all: write-tier
       * grants are administered against aimee-kb, where the DB layer's
       * admin-or-team-lead RLS check is the authority. A route that does not
       * exist cannot be reached from either transport.
       *
       * v1_route_requires_uds now claims nothing, and this pins that it claims
       * nothing — a stale "true" would silently make some future route
       * UDS-only. */
      assert(v1_route_requires_uds("POST", "/v1/grants/write-tier") == 0);
      assert(v1_route_requires_uds("POST", "/v1/memory/store") == 0);
      assert(v1_route_requires_uds("GET", "/v1/kb/status") == 0);
      assert(v1_route_requires_uds(NULL, NULL) == 0);

      /* Route ABSENCE is not what allowed_caps answers: it is a capability/tier
       * predicate over a path, so for a path with no handler it reports "nothing
       * forbids this" and dispatch 404s separately. Asserting 0 here would be
       * asserting the wrong thing about the wrong function. The route table
       * itself is covered by server-api-conformance-check against the committed
       * route descriptor, which is where a resurrected /v1/grants route would
       * show up. */

      /* conn caps by level: data keeps CAPS_AUTHENTICATED, full grants CAPS_ALL. */
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_OFF) == CAPS_AUTHENTICATED);
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_DATA) == CAPS_AUTHENTICATED);
      assert(server_http_conn_caps(1, "plain", SERVER_REMOTE_WRITES_FULL) == CAPS_ALL);

      /* P8 thin-client posture uses the resolved per-user tier: bearer fallback
       * is query-only, a cert gains authenticated session capabilities at
       * off/data, and only a verified full grant gains CAPS_ALL. */
      uint32_t fallback = CAPS_READ_ONLY & ~(uint32_t)CAP_CHAT;
      assert(server_http_effective_conn_caps(1, "plain", SERVER_REMOTE_WRITES_FULL, 1, 0) ==
             fallback);
      /* The cutover metric reconstructs the retired global switch. It must not
       * reuse today's optional-mTLS fallback caps or the denied write vanishes
       * from remote_writes.global_ignored. */
      assert(server_http_retired_global_would_allow(-1, 1, "plain", SERVER_REMOTE_WRITES_FULL, 1, 0,
                                                    "POST", "/v1/memory/store") == 1);
      assert(server_http_retired_global_would_allow(-1, 1, "plain", SERVER_REMOTE_WRITES_OFF, 1, 0,
                                                    "POST", "/v1/memory/store") == 0);
      assert(server_http_effective_conn_caps(1, "plain", SERVER_REMOTE_WRITES_OFF, 1, 1) ==
             CAPS_AUTHENTICATED);
      assert(server_http_effective_conn_caps(1, "plain", SERVER_REMOTE_WRITES_DATA, 1, 1) ==
             CAPS_AUTHENTICATED);
      assert(server_http_effective_conn_caps(1, "plain", SERVER_REMOTE_WRITES_FULL, 1, 1) ==
             CAPS_ALL);
      assert(server_http_route_allowed_caps(
                 1, server_http_effective_conn_caps(1, "plain", SERVER_REMOTE_WRITES_FULL, 1, 1),
                 "POST", "/v1/kb/build", SERVER_REMOTE_WRITES_FULL) == 1);
      assert(server_http_mtls_transport_allowed(1, 1, 0, "GET", "/v1/config") == 1);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "GET", "/v1/config") == 0);
      assert(server_http_mtls_transport_allowed(1, 2, 1, "GET", "/v1/config") == 1);
      assert(server_http_mtls_transport_allowed(0, 2, 0, "GET", "/v1/config") == 1);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "POST", "/v1/cert/sign") == 1);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "POST", "/v1/api/enroll_bearer") == 1);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "POST", "/v1/api/rotate_bearer") == 1);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "GET", "/v1/cert/sign") == 0);
      assert(server_http_mtls_transport_allowed(1, 2, 0, "POST", "/v1/cert/sign/extra") == 0);
      assert(server_http_route_allowed_caps(1, fallback, "POST", "/v1/memory/store",
                                            SERVER_REMOTE_WRITES_OFF) == 0);
      assert(server_http_route_allowed_caps(1, CAPS_AUTHENTICATED, "POST", "/v1/memory/store",
                                            SERVER_REMOTE_WRITES_OFF) == 0);
      assert(server_http_route_allowed_caps(1, fallback, "POST", "/v1/chat/completions",
                                            SERVER_REMOTE_WRITES_OFF) == 0);
      assert(server_http_route_allowed_caps(1, CAPS_AUTHENTICATED, "POST", "/v1/chat/completions",
                                            SERVER_REMOTE_WRITES_OFF) == 1);
      assert(server_http_route_allowed_caps(1, CAPS_READ_ONLY, "POST", "/v1/help",
                                            SERVER_REMOTE_WRITES_OFF) == 1);
      assert(server_http_route_allowed_caps(1, CAPS_READ_ONLY, "POST", "/v1/mcp/call",
                                            SERVER_REMOTE_WRITES_OFF) == 0);

      /* UDS is always full, independent of the level. */
      assert(server_http_conn_caps(0, NULL, SERVER_REMOTE_WRITES_OFF) == CAPS_ALL);
   }

   /* --- declarative route registry: dispatch --- */
   {
      char rb[1024];
      /* A self-contained public route dispatches through the table. */
      assert(server_http_route("GET", "/v1/health", NULL, 0, rb, sizeof(rb)) == 200);
      assert(strstr(rb, "aimee-server"));
      char forensic[64 * 1024];
      assert(server_http_route("GET", "/v1/server/forensics", NULL, 0, forensic,
                               sizeof(forensic)) == 200);
      assert(strstr(forensic, "\"recent_shutdowns\":"));
      /* An unknown path 404s. */
      assert(server_http_route("GET", "/v1/nope", NULL, 0, rb, sizeof(rb)) == 404);
      /* A wrong-verb known path 404s (no matching row). */
      assert(server_http_route("DELETE", "/v1/health", NULL, 0, rb, sizeof(rb)) == 404);
      /* A real route with no handler seam wired in this test returns 503, not 404
       * — proving the row matched and dispatched. */
      assert(server_http_route("GET", "/v1/rules", NULL, 0, rb, sizeof(rb)) == 503);
      openai_runs_store_reset();
      const char *roundtable_body = "{\"prompt\":\"draft\"}";
      assert(server_http_route("POST", "/v1/roundtable/review", roundtable_body,
                               (int)strlen(roundtable_body), rb, sizeof(rb)) == 200);
      assert(strstr(rb, "\"object\":\"op.run\""));
      assert(strstr(rb, "\"method\":\"roundtable.review\""));
      assert(strstr(rb, "\"status\":\"queued\""));
      for (int i = 0; i < 100 && strcmp(g_disp_method, "roundtable.review") != 0; i++)
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

      /* A pooled orchestration worker must begin every op with empty checkout
       * and credential TLS, even when the previous op leaked both. This is the
       * roundtable artifact/Codex-seat isolation boundary under concurrency.
       * Keep the normal four-worker coverage above, then use a fresh one-worker
       * pool solely to guarantee that poison and inspect reuse one thread. */
      compute_pool_shutdown(&g_test_server_ctx.orchestration_pool);
      assert(compute_pool_init(&g_test_server_ctx.orchestration_pool, 1) == 0);
      atomic_store(&g_op_context_clean, 0);
      submit_and_wait_op("test.poison_op_context");
      submit_and_wait_op("test.inspect_op_context");
      assert(atomic_load(&g_op_context_clean) == 1);
      openai_runs_store_reset();

      assert(server_http_submit_op_run("roundtable.review", "{\"prompt\":\"draft\"}",
                                       CAP_TOOL_EXECUTE, rb, sizeof(rb)) == 403);
      assert(strstr(rb, "insufficient capabilities"));
      g_test_server_ctx_available = 0;
      assert(server_http_submit_op_run("roundtable.review", "{\"prompt\":\"draft\"}", CAP_DELEGATE,
                                       rb, sizeof(rb)) == 503);
      assert(strstr(rb, "orchestration unavailable"));
      g_test_server_ctx_available = 1;
      compute_pool_close(&g_test_server_ctx.orchestration_pool);
      assert(server_http_submit_op_run("roundtable.review", "{\"prompt\":\"draft\"}", CAP_DELEGATE,
                                       rb, sizeof(rb)) == 503);
      assert(strstr(rb, "orchestration unavailable"));
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
      assert(strstr(report, "server loopback"));
      assert(strstr(report, "ssh -L 8910:127.0.0.1:8910 <server-host>"));
      assert(strstr(report, "model aimee"));
      assert(strstr(report, "Continue"));
      assert(strstr(report, "Copilot"));
      assert(strstr(report, "configured"));
      assert(strstr(report, "60 req/min"));
      /* Recommends a project-scoped bearer for the editor. */
      assert(strstr(report, "scope:project:<id>:<secret>"));

      /* A configured retired global is visible before the first refusal. The
       * live rig uses this as proof that its full-mode server actually started
       * with the value it intends to exercise. It still grants no authority. */
      g_remote_writes = SERVER_REMOTE_WRITES_FULL;
      server_http_api_status_report(8910, 1, 60, report, sizeof(report));
      assert(strstr(report, "aimee.api.remote_writes NO LONGER AUTHORIZES"));
      assert(strstr(report, "remote_writes.global_ignored"));
      g_remote_writes = SERVER_REMOTE_WRITES_OFF;

      /* Missing bearer is called out (the listener refuses to bind without it). */
      server_http_api_status_report(8910, 0, 0, report, sizeof(report));
      assert(strstr(report, "NOT configured"));
      assert(strstr(report, "unlimited"));

      /* Disabled listener: explains how to turn it on, no provider snippets. */
      server_http_api_status_report(0, 0, 0, report, sizeof(report));
      assert(strstr(report, "disabled"));
      assert(strstr(report, "aimee api enable"));
      assert(strstr(report, "directly into Vault"));
      assert(!strstr(report, "bearer_token:"));
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
          {"POST", "/v1/model/add", "{\"name\":\"a\"}", "model.add"},
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

   /* Negotiated buffered responses use real RFC 1952 gzip framing and remain
    * byte-equivalent after bounded decoding. */
   {
      int pair[2];
      assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
      char body[8193];
      for (size_t i = 0; i < sizeof(body) - 1; i++)
         body[i] = (char)('a' + ((i * 31 + (i / 97) * 7) % 26));
      body[sizeof(body) - 1] = '\0';
      server_http_gzip_set(1);
      send_response(pair[0], 200, body, "gzip-test");
      close(pair[0]);
      unsigned char wire[16385];
      size_t used = 0;
      for (;;)
      {
         ssize_t n = read(pair[1], wire + used, sizeof(wire) - used - 1);
         if (n <= 0)
            break;
         used += (size_t)n;
      }
      close(pair[1]);
      wire[used] = '\0';
      char *payload = strstr((char *)wire, "\r\n\r\n");
      assert(payload && strstr((char *)wire, "Content-Encoding: gzip\r\n"));
      assert(strstr((char *)wire, "Accept-Request-Encoding: gzip\r\n"));
      payload += 4;
      unsigned char *decoded = NULL;
      size_t decoded_len = 0;
      assert(http_gzip_decompress(payload, used - (size_t)(payload - (char *)wire), 1u << 20, 1000,
                                  &decoded, &decoded_len) == 0);
      assert(decoded_len == strlen(body) && memcmp(decoded, body, decoded_len) == 0);
      free(decoded);
      server_http_gzip_set(0);
   }

   /* Every data-plane request accepts one unambiguous HTTP/1.1 frame and
    * rejects duplicate lengths, transfer coding, obs-fold, and pipelining. */
   {
      const char *valid = "GET /v1/health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
      const char *valid_no_length = "OPTIONS /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
      const char *partial =
          "POST /v1/responses HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n{}";
      const char *route_oversize =
          "POST /v1/responses HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4194305\r\n\r\n";
      const char *length_overflow = "POST /v1/responses HTTP/1.1\r\nHost: localhost\r\n"
                                    "Content-Length: 18446744073709551616\r\n\r\n";
      const char *duplicate =
          "POST /v1/responses HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx";
      const char *chunked =
          "POST /v1/responses HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
      const char *conflicting_connection =
          "GET /v1/health HTTP/1.1\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n";
      const char *folded = "GET /v1/health HTTP/1.1\r\n X-folded: bad\r\n\r\n";
      const char *pipelined =
          "GET /v1/health HTTP/1.1\r\nContent-Length: 0\r\n\r\nGET /v1/health HTTP/1.1\r\n\r\n";
      assert(server_http_request_framing_valid(valid, strlen(valid)) == 1);
      assert(server_http_request_framing_valid(valid_no_length, strlen(valid_no_length)) == 1);
      assert(server_http_request_framing_valid(partial, strlen(partial)) == 1);
      assert(server_http_request_framing_valid(route_oversize, strlen(route_oversize)) == 1);
      assert(server_http_request_framing_valid(length_overflow, strlen(length_overflow)) == 0);
      assert(server_http_request_framing_valid(duplicate, strlen(duplicate)) == 0);
      assert(server_http_request_framing_valid(chunked, strlen(chunked)) == 0);
      assert(server_http_request_framing_valid(conflicting_connection,
                                               strlen(conflicting_connection)) == 0);
      assert(server_http_request_framing_valid(folded, strlen(folded)) == 0);
      assert(server_http_request_framing_valid(pipelined, strlen(pipelined)) == 0);
   }

   /* The wizard creates one durable identity transaction: additive bearer now,
    * explicit full tier only after the CSR certificate is bound. */
   {
      assert(db1_init(":memory:") == 0);
      assert(runtime_secret_store("AIMEE_API_BEARER_TOKEN", "primary") == 0);
      assert(config_set_server_api_mtls(1) == 0);
      for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
      {
         char name[96];
         snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
         runtime_secret_remove(name);
      }

      pthread_barrier_t barrier;
      pthread_t workers[2];
      wizard_bootstrap_thread_t attempts[2] = {{.barrier = &barrier}, {.barrier = &barrier}};
      assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
      assert(pthread_create(&workers[0], NULL, wizard_bootstrap_thread, &attempts[0]) == 0);
      assert(pthread_create(&workers[1], NULL, wizard_bootstrap_thread, &attempts[1]) == 0);
      int barrier_result = pthread_barrier_wait(&barrier);
      assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
      assert(pthread_join(workers[0], NULL) == 0);
      assert(pthread_join(workers[1], NULL) == 0);
      assert(pthread_barrier_destroy(&barrier) == 0);
      assert(attempts[0].result == 0 && attempts[1].result == 0);
      assert(strcmp(attempts[0].bearer, attempts[1].bearer) == 0);

      char bearer[65], again[65], principal[128];
      snprintf(bearer, sizeof(bearer), "%s", attempts[0].bearer);
      assert(strlen(bearer) == 64 && server_http_enrolled_bearer_count() == 1);
      assert(config_server_api_bearer_extra_count() == 1);
      assert(strcmp(config_server_api_bearer_extra(0), bearer) == 0);
      assert(server_http_authorize_enrolled(1, "primary", NULL, bearer, 0) == 0);
      assert(server_http_first_user_bootstrap("webuser:alice", again, sizeof(again)) == 0);
      assert(strcmp(again, bearer) == 0); /* refresh is idempotent */
      assert(server_http_first_user_bootstrap("webuser:bob", again, sizeof(again)) == -2);

      assert(server_http_first_user_cert_tier("A1B2", principal, sizeof(principal)) == 0);
      int effective_tier = SERVER_REMOTE_WRITES_OFF;
      assert(server_http_first_user_apply_cert_grant(0, "A1B2", &effective_tier, principal,
                                                     sizeof(principal)) == 0);
      assert(effective_tier == SERVER_REMOTE_WRITES_OFF && !principal[0]);
      assert(server_http_first_user_bind_cert(bearer, "A1B2") == 1);
      assert(server_http_first_user_cert_tier("A1B2", principal, sizeof(principal)) == 2);
      assert(strcmp(principal, "webuser:alice") == 0);
      effective_tier = SERVER_REMOTE_WRITES_OFF;
      assert(server_http_first_user_apply_cert_grant(1, "A1B2", &effective_tier, principal,
                                                     sizeof(principal)) == 2);
      assert(effective_tier == SERVER_REMOTE_WRITES_FULL);
      assert(strcmp(principal, "webuser:alice") == 0);
      assert(server_http_first_user_bootstrap("webuser:alice", again, sizeof(again)) == 1);
      runtime_secret_remove("AIMEE_API_BEARER_TOKEN");
      runtime_secret_remove("AIMEE_API_BEARER_TOKEN_EXTRA_0");
      db1_shutdown();
   }

   compute_pool_shutdown(&g_test_server_ctx.orchestration_pool);
   g_test_server_ctx.orchestration_pool_initialized = 0;
   platform_test_rmrf(home);
   printf("OK\n");
   return 0;
}

/* The kb_client transport, stubbed. This test links the /v1 route table, which now
 * references the grant handlers, and those call kb over HTTP. The test never invokes them —
 * its concern is the route gate, not the handler bodies — so refusing stubs are both
 * sufficient and the safer default: if a grant handler is ever reached from here by
 * accident, it fails closed rather than proceeding against a fabricated kb.
 *
 * The handlers' own behaviour is covered by test_kb_client_grants.c (interpretation of kb's
 * answers) and test_kb_http_grants.c (kb's side). */
char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   (void)path;
   (void)body;
   (void)timeout_ms;
   if (status_out)
      *status_out = 0;
   return NULL;
}

char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out)
{
   (void)path;
   (void)timeout_ms;
   if (status_out)
      *status_out = 0;
   return NULL;
}

char *kb_client_query_escape(const char *s)
{
   (void)s;
   return NULL;
}

char *kb_client_project_status_json(const char *project)
{
   char buf[256];
   snprintf(buf, sizeof(buf), "{\"project\":\"%s\",\"chunks\":7,\"vector_points\":7}",
            project ? project : "");
   return strdup(buf);
}

char *kb_client_ingest_status_json(void)
{
   return strdup("{\"queue\":{\"pending\":0,\"running\":0},\"workers\":{\"configured\":1}}");
}

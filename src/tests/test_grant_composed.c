/* test_grant_composed.c — the grant commands through the SHIPPING routing boundary.
 *
 * WHY THIS EXISTS, in the reviewer's words rather than mine: every other test for this
 * feature stubs either the transport or the database, and the one "live" run used a
 * pre-change server that answered 404. So no successful CLI-to-server-to-kb path had ever
 * been exercised — and a review then found a defect that layer-local testing could not see:
 * `aimee kb grant show` with no --subject silently issued an UNFILTERED LIST, because `show`
 * and `list` shared a method and the marshaller could not tell them apart.
 *
 * Each layer was individually correct. The COMPOSITION was not. That is the gap this closes.
 *
 * What is real here and what is not:
 *   REAL — marshal_request (the CLI's flags-to-body step), cli_v1_route_for_method (method to
 *          path and verb), server_http_route_allowed_caps (the UDS gate), and the server's
 *          route handlers.
 *   STUBBED — only kb_client, the bottom of the stack. Everything the request passes through
 *          on its way there is the shipping code.
 *
 * So a mismatch between ANY two adjacent layers fails here: a method with no path, a path
 * with no route, a body the handler cannot read, a field name that drifted, or a command
 * whose identity is lost in marshalling.
 */
#include "cJSON.h"
#include "cli_client.h"
#include "cli_v1_routes_internal.h" /* marshal_request: the CLI's flags-to-body step */
#include "kb_client_grants.h"
#include "server.h"
#include "server_http.h"
#include "server_http_internal.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── kb_client, stubbed: the bottom of the stack and nothing above it ─────── */

static kb_client_grant_result_t stub_rc;
static char stub_seen_server[128];
static char stub_seen_subject[600];
static char stub_seen_tier[16];
static int64_t stub_seen_team;
static int stub_seen_include_revoked;
static int stub_seen_had_subject;
static int stub_set_calls, stub_revoke_calls, stub_list_calls;
static kb_client_grant_change_t stub_change;
static int stub_found;
static size_t stub_row_count;
static kb_client_grant_row_t stub_rows[4];

kb_client_grant_result_t kb_client_grant_set(const char *server_id, int64_t team_id,
                                             const char *subject, const char *tier,
                                             const char *granted_by, kb_client_grant_change_t *out)
{
   stub_set_calls++;
   snprintf(stub_seen_server, sizeof(stub_seen_server), "%s", server_id ? server_id : "");
   snprintf(stub_seen_subject, sizeof(stub_seen_subject), "%s", subject ? subject : "");
   snprintf(stub_seen_tier, sizeof(stub_seen_tier), "%s", tier ? tier : "");
   stub_seen_team = team_id;
   /* The granter must be the server's own notion of the local operator, never something the
    * request supplied — otherwise the audit trail can be written to order. */
   assert(granted_by && !strcmp(granted_by, "owner"));
   if (out)
      *out = stub_change;
   return stub_rc;
}

kb_client_grant_result_t kb_client_grant_revoke(const char *server_id, int64_t team_id,
                                                const char *subject, int *found_out)
{
   stub_revoke_calls++;
   snprintf(stub_seen_server, sizeof(stub_seen_server), "%s", server_id ? server_id : "");
   snprintf(stub_seen_subject, sizeof(stub_seen_subject), "%s", subject ? subject : "");
   stub_seen_team = team_id;
   if (found_out)
      *found_out = stub_found;
   return stub_rc;
}

kb_client_grant_result_t kb_client_grant_list(const char *server_id, int64_t team_id,
                                              const char *subject, int include_revoked,
                                              kb_client_grant_row_t *out, size_t cap, size_t *count,
                                              int *truncated_out)
{
   stub_list_calls++;
   snprintf(stub_seen_server, sizeof(stub_seen_server), "%s", server_id ? server_id : "");
   stub_seen_team = team_id;
   stub_seen_include_revoked = include_revoked;
   /* Recorded so a test can prove the subject filter REACHED the bottom, which is exactly
    * what the `show` defect got wrong. */
   stub_seen_had_subject = (subject && subject[0]) ? 1 : 0;
   snprintf(stub_seen_subject, sizeof(stub_seen_subject), "%s", subject ? subject : "");
   if (truncated_out)
      *truncated_out = 0;
   size_t n = stub_row_count < cap ? stub_row_count : cap;
   for (size_t i = 0; i < n; i++)
      out[i] = stub_rows[i];
   if (count)
      *count = n;
   return stub_rc;
}

static void reset(void)
{
   stub_rc = KB_CLIENT_GRANT_OK;
   stub_set_calls = stub_revoke_calls = stub_list_calls = 0;
   stub_seen_server[0] = stub_seen_subject[0] = stub_seen_tier[0] = '\0';
   stub_seen_team = 0;
   stub_seen_include_revoked = -1;
   stub_seen_had_subject = -1;
   stub_found = 0;
   stub_row_count = 0;
   memset(&stub_change, 0, sizeof(stub_change));
   memset(stub_rows, 0, sizeof(stub_rows));
}

/* Three symbols that live in the route-table TU or the client transport, neither of which
 * this test links. Stubbing the two authz helpers makes the test STRONGER rather than weaker:
 * with the capability lookup and the data-write check neutralised, a TCP refusal below can
 * only come from v1_route_requires_uds — which is the claim, that the UDS gate fires ahead of
 * the capability and tier logic rather than relying on it. */
uint32_t v1_route_caps_lookup(const char *method, const char *path)
{
   (void)method;
   (void)path;
   return 0; /* no capability required, so only the transport gate can refuse */
}

int v1_route_is_local_only(const char *method, const char *path)
{
   (void)method;
   (void)path;
   return 0; /* historical name: "dispatches a data-write op". Not our gate. */
}

int aimee_client_remote_active_scheme(char *desc_out, unsigned long desc_sz, int *is_https_out)
{
   if (desc_out && desc_sz)
      desc_out[0] = '\0';
   if (is_https_out)
      *is_https_out = 0;
   return 0;
}

const char *aimee_home(void)
{
   return "/tmp";
}

/* err_json lives in the route-table TU, which this test deliberately does not link (see
 * tests/Rules.mk). Same shape as the real one: a JSON error body and the status. */
int err_json(char *resp, int cap, int status, const char *msg)
{
   snprintf(resp, (size_t)cap, "{\"error\":\"%s\"}", msg ? msg : "error");
   return status;
}

/* ── The composed drive ───────────────────────────────────────────────────── */

/* Run one CLI invocation the way the thin client does: marshal the flags, resolve the
 * method to a path and verb, check the transport gate, dispatch. Returns the HTTP status, or
 * -1 when marshalling refused (the CLI's silent `return 2` case) and -2 when no route
 * resolved. */
static int drive(const char *method, int argc, char **argv, int is_tcp, char *resp, int cap)
{
   cJSON *req = marshal_request(method, argc, argv);
   if (!req)
      return -1;
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -2;

   const char *verb = NULL;
   const char *path = cli_v1_route_for_method(method, &verb);
   if (!path || !verb)
   {
      free(body);
      return -2;
   }
   /* The real gate. CAPS_ALL is what a UDS caller AND a remote_writes=full TCP bearer both
    * hold, which is why the transport argument is the thing that decides. */
   if (!server_http_route_allowed_caps(is_tcp, CAPS_ALL, verb, path, SERVER_REMOTE_WRITES_FULL))
   {
      free(body);
      return 403;
   }
   /* Dispatch to the handler the table maps this path to. Chosen here rather than by linking
    * the table, which references every handler in the server; the table's own row is covered
    * by server-api-conformance-check and the committed route descriptor. */
   route_req_t rq;
   memset(&rq, 0, sizeof(rq));
   rq.method = verb;
   rq.path = path;
   rq.body = body;
   rq.body_len = (int)strlen(body);
   int status;
   if (!strcmp(path, "/v1/grants/write-tier/set"))
      status = rh_grant_set(&rq, resp, cap);
   else if (!strcmp(path, "/v1/grants/write-tier/revoke"))
      status = rh_grant_revoke(&rq, resp, cap);
   else if (!strcmp(path, "/v1/grants/write-tier/list"))
      status = rh_grant_list(&rq, resp, cap);
   else
      status = -2;
   free(body);
   return status;
}

static char **args(const char *a, ...)
{
   /* Small fixed-capacity argv builder; the tests below pass at most eight flags. */
   static char *buf[16];
   static char store[16][128];
   int n = 0;
   va_list ap;
   va_start(ap, a);
   for (const char *p = a; p && n < 16; p = va_arg(ap, const char *))
   {
      snprintf(store[n], sizeof(store[n]), "%s", p);
      buf[n] = store[n];
      n++;
   }
   va_end(ap);
   buf[n] = NULL;
   return buf;
}

static int argcount(char **argv)
{
   int n = 0;
   while (argv[n])
      n++;
   return n;
}

static void test_set(void)
{
   char resp[8192] = "";
   reset();
   stub_change.changed = 1;
   stub_change.is_member = 1;
   char **a = args("--subject", "oidc:iss:alice", "--server", "srv1", "--team", "910001", "--tier",
                   "data", NULL);
   int st = drive("kb.grant.set", argcount(a), a, 0, resp, sizeof(resp));
   if (st != 200)
   {
      fprintf(stderr, "set: status %d body %s\n", st, resp);
      assert(0);
   }
   /* Every field survived marshalling, the route, and the handler, unchanged. */
   assert(stub_set_calls == 1);
   assert(!strcmp(stub_seen_server, "srv1"));
   assert(stub_seen_team == 910001);
   assert(!strcmp(stub_seen_subject, "oidc:iss:alice"));
   assert(!strcmp(stub_seen_tier, "data"));
   assert(strstr(resp, "\"changed\":true"));
   assert(strstr(resp, "\"is_member\":true"));

   /* previous_tier round-trips only when there was one. */
   reset();
   stub_change.changed = 1;
   stub_change.had_previous = 1;
   snprintf(stub_change.previous_tier, sizeof(stub_change.previous_tier), "%s", "off");
   st = drive("kb.grant.set", argcount(a), a, 0, resp, sizeof(resp));
   assert(st == 200 && strstr(resp, "\"previous_tier\":\"off\""));
}

static void test_revoke(void)
{
   char resp[8192] = "";
   char **a = args("--subject", "oidc:iss:alice", "--server", "srv1", "--team", "910001", NULL);

   reset();
   stub_found = 1;
   assert(drive("kb.grant.revoke", argcount(a), a, 0, resp, sizeof(resp)) == 200);
   assert(stub_revoke_calls == 1 && strstr(resp, "\"found\":true"));
   assert(!strcmp(stub_seen_subject, "oidc:iss:alice") && stub_seen_team == 910001);

   reset();
   stub_found = 0;
   assert(drive("kb.grant.revoke", argcount(a), a, 0, resp, sizeof(resp)) == 200);
   assert(strstr(resp, "\"found\":false"));
}

static void test_list_and_show(void)
{
   char resp[8192] = "";

   /* list: no subject reaches the bottom, and include_revoked defaults off. */
   reset();
   snprintf(stub_rows[0].subject, sizeof(stub_rows[0].subject), "%s", "oidc:iss:alice");
   snprintf(stub_rows[0].tier, sizeof(stub_rows[0].tier), "%s", "data");
   stub_row_count = 1;
   char **a = args("--server", "srv1", "--team", "910001", NULL);
   assert(drive("kb.grant.list", argcount(a), a, 0, resp, sizeof(resp)) == 200);
   assert(stub_list_calls == 1);
   assert(stub_seen_had_subject == 0);
   assert(stub_seen_include_revoked == 0);
   assert(strstr(resp, "oidc:iss:alice"));

   /* --include-revoked reaches the bottom as a widening flag. */
   reset();
   char **b = args("--server", "srv1", "--team", "910001", "--include-revoked", NULL);
   assert(drive("kb.grant.list", argcount(b), b, 0, resp, sizeof(resp)) == 200);
   assert(stub_seen_include_revoked == 1);

   /* SHOW WITH A SUBJECT: the filter must arrive at the bottom. */
   reset();
   char **c = args("--subject", "oidc:iss:bob", "--server", "srv1", "--team", "910001", NULL);
   assert(drive("kb.grant.show", argcount(c), c, 0, resp, sizeof(resp)) == 200);
   assert(stub_list_calls == 1);
   assert(stub_seen_had_subject == 1);
   assert(!strcmp(stub_seen_subject, "oidc:iss:bob"));

   /* SHOW WITHOUT A SUBJECT MUST SEND NOTHING. This is the defect a review found: `show`
    * shared a method with `list`, so the marshaller could not require --subject and this
    * invocation issued an unfiltered list — a command naming one subject answering with
    * every grant on the server. Marshalling now refuses, so no request is made at all. */
   reset();
   char **d = args("--server", "srv1", "--team", "910001", NULL);
   int st = drive("kb.grant.show", argcount(d), d, 0, resp, sizeof(resp));
   if (st != -1)
   {
      fprintf(stderr, "show with no --subject was not refused (status %d)\n", st);
      assert(0);
   }
   assert(stub_list_calls == 0);
}

static void test_uds_only(void)
{
   char resp[8192] = "";
   /* Over TCP, with CAPS_ALL and the highest tier — exactly what a remote_writes=full bearer
    * holds — every verb is refused and NOTHING reaches kb. */
   const struct
   {
      const char *method;
      char **argv;
   } cases[] = {
       {"kb.grant.set",
        args("--subject", "owner", "--server", "s", "--team", "1", "--tier", "full", NULL)},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      reset();
      int st =
          drive(cases[i].method, argcount(cases[i].argv), cases[i].argv, 1, resp, sizeof(resp));
      assert(st == 403);
      assert(stub_set_calls == 0 && stub_revoke_calls == 0 && stub_list_calls == 0);
   }
   reset();
   char **r = args("--subject", "owner", "--server", "s", "--team", "1", NULL);
   assert(drive("kb.grant.revoke", argcount(r), r, 1, resp, sizeof(resp)) == 403);
   assert(stub_revoke_calls == 0);
   reset();
   char **l = args("--server", "s", "--team", "1", NULL);
   assert(drive("kb.grant.list", argcount(l), l, 1, resp, sizeof(resp)) == 403);
   assert(stub_list_calls == 0);
   reset();
   char **sh = args("--subject", "owner", "--server", "s", "--team", "1", NULL);
   assert(drive("kb.grant.show", argcount(sh), sh, 1, resp, sizeof(resp)) == 403);
   assert(stub_list_calls == 0);

   /* And over UDS the same invocations are permitted — a gate that refused both transports
    * would look exactly like the assertions above passing. */
   reset();
   stub_change.changed = 1;
   char **ok = args("--subject", "owner", "--server", "s", "--team", "1", "--tier", "full", NULL);
   assert(drive("kb.grant.set", argcount(ok), ok, 0, resp, sizeof(resp)) == 200);
   assert(stub_set_calls == 1);
}

static void test_kb_failures_surface(void)
{
   char resp[8192] = "";
   char **a = args("--subject", "owner", "--server", "s", "--team", "1", "--tier", "data", NULL);

   /* kb's outcomes must arrive as distinct statuses, because each implies a different next
    * action for the operator. */
   struct
   {
      kb_client_grant_result_t rc;
      int want;
   } cases[] = {{KB_CLIENT_GRANT_DENIED, 403},
                {KB_CLIENT_GRANT_BACKEND, 503},
                {KB_CLIENT_GRANT_UNAVAILABLE, 503},
                /* An unenrolled server must not read as an unreachable kb (503) nor as the
                 * caller's own credential being refused (401). */
                {KB_CLIENT_GRANT_UNAUTHENTICATED, 502},
                {KB_CLIENT_GRANT_INVALID, 400}};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      reset();
      stub_rc = cases[i].rc;
      int st = drive("kb.grant.set", argcount(a), a, 0, resp, sizeof(resp));
      if (st != cases[i].want)
      {
         fprintf(stderr, "kb rc %d gave status %d, expected %d\n", (int)cases[i].rc, st,
                 cases[i].want);
         assert(0);
      }
   }
}

/* The reported/suppressed contract on marshalling failure. Before this, EVERY command in the
 * CLI exited 2 in total silence when its arguments could not be marshalled — which reads as
 * success to a script and as nothing at all to a person. The generic message now covers that,
 * and must stay suppressed where a marshaller printed something better. */
static void test_marshal_failure_is_reported(void)
{
   /* A grant refusal explains itself, so the flag is SET and the forwarder stays quiet. */
   char **a = args("--server", "s", "--team", "1", NULL); /* show with no --subject */
   assert(marshal_request("kb.grant.show", argcount(a), a) == NULL);
   assert(marshal_request_take_reported() == 1);
   /* Reading it CLEARED it, so a stale flag cannot silence the next command. */
   assert(marshal_request_take_reported() == 0);

   /* A method with no specific message leaves it unset, so the generic one prints. */
   char **b = args("--server", "s", NULL); /* list with no --team */
   assert(marshal_request("kb.grant.list", argcount(b), b) == NULL);
   assert(marshal_request_take_reported() == 1); /* list's team check also explains */

   /* A SUCCESSFUL marshal must not leave the flag set, or the next failure would be silent. */
   char **c = args("--subject", "owner", "--server", "s", "--team", "1", "--tier", "data", NULL);
   cJSON *req = marshal_request("kb.grant.set", argcount(c), c);
   assert(req != NULL);
   cJSON_Delete(req);
   assert(marshal_request_take_reported() == 0);
}

static void test_marshal_refusals_send_nothing(void)
{
   char resp[8192] = "";
   /* Each of these is refused before a request exists. The assertion that matters is that
    * NOTHING reached kb — a late refusal at the handler would also return non-200 while
    * having already asked. */
   struct
   {
      const char *method;
      char **argv;
   } bad[] = {
       {"kb.grant.set", args("--server", "s", "--team", "1", "--tier", "data", NULL)},
       {"kb.grant.set", args("--subject", "owner", "--team", "1", "--tier", "data", NULL)},
       {"kb.grant.set", args("--subject", "owner", "--server", "s", "--tier", "data", NULL)},
       {"kb.grant.set", args("--subject", "owner", "--server", "s", "--team", "1", NULL)},
       {"kb.grant.set",
        args("--subject", "owner", "--server", "s", "--team", "1", "--tier", "root", NULL)},
       {"kb.grant.set",
        args("--subject", "owner", "--server", "s", "--team", "77x", "--tier", "data", NULL)},
       {"kb.grant.set",
        args("--subject", "owner", "--server", "s", "--team", "0", "--tier", "data", NULL)},
       {"kb.grant.revoke", args("--server", "s", "--team", "1", NULL)},
       {"kb.grant.show", args("--server", "s", "--team", "1", NULL)},
       {"kb.grant.list", args("--server", "s", NULL)},
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      reset();
      int st = drive(bad[i].method, argcount(bad[i].argv), bad[i].argv, 0, resp, sizeof(resp));
      if (st != -1 || stub_set_calls || stub_revoke_calls || stub_list_calls)
      {
         fprintf(stderr, "%s case %zu was not refused before the request (status %d)\n",
                 bad[i].method, i, st);
         assert(0);
      }
   }
}

static void test_every_method_resolves(void)
{
   /* A method with no path, or a path with no route, would make every test above fail in a
    * confusing way. Asserted directly so the failure names the cause. */
   const char *methods[] = {"kb.grant.set", "kb.grant.revoke", "kb.grant.list", "kb.grant.show"};
   for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
   {
      const char *verb = NULL;
      const char *path = cli_v1_route_for_method(methods[i], &verb);
      if (!path || !verb)
      {
         fprintf(stderr, "%s resolves to no /v1 route\n", methods[i]);
         assert(0);
      }
      /* All three are POST, including the read: the thin client marshals into a body. */
      assert(!strcmp(verb, "POST"));
      assert(!strncmp(path, "/v1/grants/write-tier", 21));
      /* And each is UDS-only, so a route added to the family cannot quietly be remote. */
      assert(v1_route_requires_uds(verb, path) == 1);
   }
   /* show and list share the ROUTE but not the METHOD — that separation is what lets the
    * marshaller require a subject for one and not the other. */
   const char *lv = NULL, *sv = NULL;
   const char *lp = cli_v1_route_for_method("kb.grant.list", &lv);
   const char *sp = cli_v1_route_for_method("kb.grant.show", &sv);
   assert(lp && sp && !strcmp(lp, sp));
}

int main(void)
{
   test_every_method_resolves();
   test_set();
   test_revoke();
   test_list_and_show();
   test_uds_only();
   test_kb_failures_surface();
   test_marshal_refusals_send_nothing();
   test_marshal_failure_is_reported();
   printf("test_grant_composed: ok\n");
   return 0;
}

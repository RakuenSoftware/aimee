/* test_cli_v1_subcommands.c — cli_v1_subcommands(), which turns a failed route
 * lookup into an actionable message.
 *
 * Before this helper existed, `aimee economizer status` reported
 * "command 'economizer' has no /v1 route; add a /v1 route" — blaming the command
 * (and telling the user to add a route) when the command is routed fine and only
 * the SUBCOMMAND was wrong. That message sends people hunting for a missing route
 * that already exists.
 *
 * The first cut of the helper walked rpc_routes with sizeof/sizeof, which runs
 * off the end into the table's {NULL,...} sentinel and segfaults in strcmp. The
 * unknown-command case below is the regression guard for that crash. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "cli_client.h"
#include "cli_v1_routes_internal.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* A command that is not in the table at all: must report zero and must not walk
 * into the NULL sentinel. This is the segfault guard. */
static void test_unknown_command_is_safe(void)
{
   char buf[256] = "sentinel";
   assert(cli_v1_subcommands("no-such-command-anywhere", buf, sizeof(buf)) == 0);
   assert(buf[0] == '\0');

   /* every rejected lookup path, including the ones a crash would take */
   assert(cli_v1_subcommands("", buf, sizeof(buf)) == 0);
   assert(cli_v1_subcommands(NULL, buf, sizeof(buf)) == 0);
   printf("  unknown command is safe (no sentinel walk)\n");
}

/* A routed family reports its real subcommands. */
static void test_known_command_lists_subcommands(void)
{
   char buf[512];
   int n = cli_v1_subcommands("economizer", buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "stats") != NULL);

   n = cli_v1_subcommands("kb", buf, sizeof(buf));
   assert(n >= 4);
   assert(strstr(buf, "search") != NULL);
   assert(strstr(buf, "build") != NULL); /* the row whose absence made kb build unreachable */
   assert(strstr(buf, "status") != NULL);

   n = cli_v1_subcommands("curator", buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "contradictions") != NULL);
   printf("  known commands list their subcommands\n");
}

/* Entries are comma-separated with no leading/trailing separator. */
static void test_list_formatting(void)
{
   char buf[512];
   assert(cli_v1_subcommands("kb", buf, sizeof(buf)) > 0);
   assert(buf[0] != ',');
   assert(buf[0] != ' ');
   size_t len = strlen(buf);
   assert(len > 0);
   assert(buf[len - 1] != ',');
   assert(buf[len - 1] != ' ');
   assert(strstr(buf, ",,") == NULL);
   printf("  list formatting ok\n");
}

/* A tiny buffer must truncate, never overflow. The count still reports the true
 * number of subcommands so the caller can tell the list was clipped. */
static void test_small_buffer_does_not_overflow(void)
{
   char guard[64];
   memset(guard, 0x7f, sizeof(guard));
   char *buf = guard + 16; /* poisoned bytes on both sides */
   int n = cli_v1_subcommands("kb", buf, 8);
   assert(n > 0);
   assert(strlen(buf) < 8);
   for (int i = 0; i < 16; i++)
      assert((unsigned char)guard[i] == 0x7f); /* nothing written before */
   for (size_t i = 16 + 8; i < sizeof(guard); i++)
      assert((unsigned char)guard[i] == 0x7f); /* nothing written past cap */

   /* a zero-cap / NULL out must still count without writing */
   assert(cli_v1_subcommands("kb", NULL, 0) > 0);
   printf("  small buffer truncates without overflow\n");
}

/* Rows that are NULL (match-any) or "" (bare command) are not typeable names and
 * must not appear in the suggestion list. `workers` is registered only as a bare
 * command, so it contributes no subcommand names. */
static void test_bare_and_matchany_rows_excluded(void)
{
   char buf[256];
   int n = cli_v1_subcommands("workers", buf, sizeof(buf));
   assert(n == 0);
   assert(buf[0] == '\0');
   printf("  bare / match-any rows excluded\n");
}

/* `aimee kb status` against a remote server dropped the three fields that carry
 * bad news. The payload below is the real one observed from a deployment whose
 * kb had a 5601-job backlog with 75 failures: it rendered as
 *
 *   project: aimee
 *   chunks:        0
 *   Background ingest: 0 pending, 0 done last 24h
 *
 * i.e. idle and healthy. ingest_queue is a genuinely different queue and was
 * genuinely 0; the backlog lives in `queue`, which was never read. */
static void test_kb_status_reports_backlog_and_degradation(void)
{
   static const char *payload =
       "{\"summary_status\":\"degraded\",\"project\":\"aimee\",\"files\":0,\"chunks\":0,"
       "\"queue\":{\"pending\":5601,\"running\":0,\"done\":362,\"failed\":75,\"total\":6038},"
       "\"vector\":{\"memory_points\":640,\"kb_points\":5983,\"status\":\"ok\"},"
       "\"ingest_queue\":{\"pending\":0,\"running\":0,\"done_last_24h\":0}}";
   cJSON *resp = cJSON_Parse(payload);
   assert(resp);

   char path[256];
   snprintf(path, sizeof path, "%s/aimee-kbstatus-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   assert(saved >= 0);
   assert(dup2(fd, STDOUT_FILENO) >= 0);

   pt_print_kb_status("kb.status", resp);

   fflush(stdout);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   close(fd);

   FILE *f = fopen(path, "r");
   assert(f);
   char out[2048] = "";
   size_t n = fread(out, 1, sizeof(out) - 1, f);
   out[n] = '\0';
   fclose(f);
   unlink(path);
   cJSON_Delete(resp);

   /* The backlog and the failures must both be visible. */
   assert(strstr(out, "5601") != NULL);
   assert(strstr(out, "75") != NULL);
   /* The server's own verdict must not be silently dropped. */
   assert(strstr(out, "degraded") != NULL);
   /* Vector points live nested under `vector`, not at the top level. */
   assert(strstr(out, "5983") != NULL);
   assert(strstr(out, "640") != NULL);
   printf("  kb status surfaces backlog, failures, and degraded verdict\n");
}

/* Capture one printer's stdout into `out`. */
static void capture_printer(void (*printer)(const char *, cJSON *), const char *method,
                            const char *payload, char *out, size_t out_n)
{
   cJSON *resp = cJSON_Parse(payload);
   assert(resp);
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-v1print-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   assert(saved >= 0);
   assert(dup2(fd, STDOUT_FILENO) >= 0);

   printer(method, resp);

   fflush(stdout);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   close(fd);

   FILE *f = fopen(path, "r");
   assert(f);
   size_t n = fread(out, 1, out_n - 1, f);
   out[n] = '\0';
   fclose(f);
   unlink(path);
   cJSON_Delete(resp);
}

/* `aimee agent roles <name>` and `aimee agent personas <name>` MUTATE the agent:
 * with no csv argument they reset it to the full default set. Both were wired to
 * the agent.enable printer, so the operator's only feedback was
 *
 *   Delegate 'codex' enabled.
 *
 * which names the wrong operation and hides the roles that were just written —
 * a silent reset that drops any role outside the default list (`all`, and any
 * operator-added role). The write is intentional; reporting it as "enabled" is
 * not. Print what the server actually stored. */
static void test_agent_roles_printer_reports_roles(void)
{
   static const char *payload =
       "{\"name\":\"codex\",\"status\":\"ok\","
       "\"roles\":[\"code\",\"review\",\"validate\",\"all\"],\"personas\":[\"all\"]}";
   char out[1024] = "";
   capture_printer(pt_print_agent_roles, "model.roles", payload, out, sizeof(out));

   assert(strstr(out, "codex") != NULL);
   /* The roles that were written must be visible... */
   assert(strstr(out, "code") != NULL);
   assert(strstr(out, "validate") != NULL);
   assert(strstr(out, "all") != NULL);
   /* ...and the operation must not be misreported as an enable. */
   assert(strstr(out, "enabled") == NULL);
   /* A csv-bearing call mutated, so "set to" is accurate here. */
   assert(strstr(out, "set to") != NULL);
   printf("  agent.roles prints the stored roles, not \"enabled\"\n");
}

/* A bare `agent roles <name>` only reports. Saying "set to" would claim a write
 * that did not happen — the same class of mistake as reporting it as "enabled". */
static void test_agent_roles_printer_read_is_not_reported_as_a_write(void)
{
   static const char *payload = "{\"name\":\"codex\",\"read_only\":true,"
                                "\"roles\":[\"code\",\"diagnose\",\"all\"]}";
   char out[1024] = "";
   capture_printer(pt_print_agent_roles, "model.roles", payload, out, sizeof(out));

   assert(strstr(out, "codex") != NULL);
   assert(strstr(out, "diagnose") != NULL);
   assert(strstr(out, "all") != NULL);
   assert(strstr(out, "set to") == NULL);
   assert(strstr(out, "enabled") == NULL);
   printf("  agent.roles read does not claim a write\n");
}

static void test_agent_personas_printer_reports_personas(void)
{
   static const char *payload = "{\"name\":\"codex\",\"status\":\"ok\","
                                "\"roles\":[\"code\"],\"personas\":[\"engineer\",\"qa\"]}";
   char out[1024] = "";
   capture_printer(pt_print_agent_personas, "model.personas", payload, out, sizeof(out));

   assert(strstr(out, "codex") != NULL);
   assert(strstr(out, "engineer") != NULL);
   assert(strstr(out, "qa") != NULL);
   assert(strstr(out, "enabled") == NULL);
   printf("  agent.personas prints the stored personas, not \"enabled\"\n");
}

/* `aimee delegate --list-roles` is routed to agent.list, so the roster printer is
 * what has to show the roles. It printed only tier/parallel/model/endpoint, which
 * meant the flag documented as "List available roles" listed no role at all. */
static void test_agent_list_printer_shows_roles(void)
{
   static const char *payload =
       "{\"agents\":[{\"name\":\"codex\",\"enabled\":true,\"cost_tier\":0,\"max_parallel\":3,"
       "\"model\":\"gpt-5.5\",\"endpoint\":\"https://example.invalid\",\"tools_enabled\":true,"
       "\"roles\":[\"code\",\"review\",\"validate\"]}]}";
   char out[2048] = "";
   capture_printer(pt_print_agent_list, "model.list", payload, out, sizeof(out));

   assert(strstr(out, "codex") != NULL);
   assert(strstr(out, "roles:") != NULL);
   assert(strstr(out, "code") != NULL);
   assert(strstr(out, "review") != NULL);
   assert(strstr(out, "validate") != NULL);
   printf("  agent.list prints each agent's roles\n");
}

/* Async dispatch preserves an ordinary handler error under run.result. The CLI
 * must surface that useful cause instead of replacing it with the generic
 * "run failed with no result". */
static void test_run_failure_reports_dispatch_message(void)
{
   cJSON *result =
       cJSON_Parse("{\"status\":\"error\",\"message\":\"knowledge service unavailable\"}");
   cJSON *snapshot = cJSON_Parse("{\"status\":\"failed\"}");
   assert(result && snapshot);
   assert(strcmp(cli_v1_run_failure_reason(result, snapshot), "knowledge service unavailable") ==
          0);
   cJSON_Delete(result);
   cJSON_Delete(snapshot);
   printf("  async run preserves dispatch error message\n");
}

/* A gated method must not reach the wire without consent, and an ungated one must be
 * untouched. The gate is client-side because the server has no terminal to ask at, so
 * this is the layer testable without one: with no --confirm and no tty, cli_v1_forward
 * must refuse before it marshals anything.
 *
 * `aimee workspace remove <path>` used to drop the registration on the spot, with no
 * prompt and no mention that the indexed corpus stays searchable afterwards. */
static void test_gated_method_refuses_without_confirmation(void)
{
   /* stdin under the test runner is not a terminal, which is exactly the
    * non-interactive case the gate must refuse rather than assume consent for. */
   assert(!isatty(STDIN_FILENO));

   cli_v1_route_t route;
   char *argv_remove[] = {"remove", "some-workspace"};
   assert(cli_v1_lookup("workspace", 2, argv_remove, &route) == 1);
   assert(strcmp(route.method, "workspace.remove") == 0);
   /* 2 = refused before dispatch. A 0 here would mean it was sent. */
   assert(cli_v1_forward(NULL, &route, 0, NULL, NULL, 2, argv_remove) == 2);

   /* --confirm gets past the gate; it then fails on transport, which is a different
    * and later failure than the refusal above. */
   char *argv_confirm[] = {"remove", "some-workspace", "--confirm"};
   assert(cli_v1_lookup("workspace", 3, argv_confirm, &route) == 1);
   assert(cli_v1_forward(NULL, &route, 0, NULL, NULL, 3, argv_confirm) != 2);

   /* An ungated sibling is unaffected: no prompt, no refusal. */
   char *argv_list[] = {"list"};
   assert(cli_v1_lookup("workspace", 1, argv_list, &route) == 1);
   assert(cli_v1_forward(NULL, &route, 0, NULL, NULL, 1, argv_list) != 2);

   printf("  workspace remove refuses without --confirm on a non-tty\n");
}

/* An unquoted memory body must not be truncated to its first word.
 *
 * `aimee memory store <key> <content>` read positional[1] and nothing else, so
 * a caller who forgot to quote the content -- which arrives as one positional
 * PER WORD -- stored the first word and lost the rest. It exited 0 and printed
 * "stored memory 60", so nothing said the memory had been gutted.
 *
 * Observed live on a deployment: storing a 16-word fact stored the single word
 * "The". The loss is silent and on the WRITE path, so it is not discovered when
 * it happens; it is discovered later as a memory that does not say what was
 * meant, or as a search that cannot find what was stored.
 *
 * memory identity / prefer shared the defect through marshal_user_capture. */
static void test_memory_store_keeps_unquoted_content(void)
{
   char *argv[] = {(char *)"k", (char *)"one", (char *)"two", (char *)"three", (char *)"four"};
   cJSON *req = marshal_memory_store(5, argv);
   assert(req);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(req, "content");
   assert(cJSON_IsString(content));
   assert(strcmp(content->valuestring, "one two three four") == 0);
   cJSON *key = cJSON_GetObjectItemCaseSensitive(req, "key");
   assert(cJSON_IsString(key) && strcmp(key->valuestring, "k") == 0);
   cJSON_Delete(req);

   /* The ordinary quoted form is unchanged: one positional stays one value. */
   char *quoted[] = {(char *)"k", (char *)"one two three four"};
   req = marshal_memory_store(2, quoted);
   assert(req);
   content = cJSON_GetObjectItemCaseSensitive(req, "content");
   assert(cJSON_IsString(content));
   assert(strcmp(content->valuestring, "one two three four") == 0);
   cJSON_Delete(req);

   /* The --content flag still wins when there is no positional body. */
   char *flagged[] = {(char *)"k", (char *)"--content=from the flag"};
   req = marshal_memory_store(2, flagged);
   assert(req);
   content = cJSON_GetObjectItemCaseSensitive(req, "content");
   assert(cJSON_IsString(content));
   assert(strcmp(content->valuestring, "from the flag") == 0);
   cJSON_Delete(req);

   /* identity shares the marshaller family and the same fix. */
   char *ident[] = {(char *)"role", (char *)"staff", (char *)"platform", (char *)"engineer"};
   req = marshal_memory_identity(4, ident);
   assert(req);
   content = cJSON_GetObjectItemCaseSensitive(req, "content");
   assert(cJSON_IsString(content));
   assert(strcmp(content->valuestring, "staff platform engineer") == 0);
   cJSON_Delete(req);

   printf("  unquoted memory content is kept whole, not truncated to word one\n");
}

/* A pending count with nothing draining it must SAY so on `aimee kb status`.
 *
 * memory.store enqueues a memory_facts job whenever typed facts are on (the
 * default); the only consumer runs on the curator LLM lane, which deliberately
 * does not start without a synthesis endpoint. Both decisions are right, and
 * together they mean a supported configuration queues one row per stored memory
 * that nothing will ever claim. The warning for it was written into
 * kb_service_health_object() while its own comment named the symptom on the
 * OTHER surface -- so the command an operator runs kept printing a bare number.
 *
 * Reproduced live: 65 jobs, oldest 34 hours, attempts 0, under "status: ok".
 * An undrainable queue and a busy one print the same pending count; only this
 * line distinguishes them, so it must survive a verdict that stays ok. */
static void test_kb_status_warns_about_undrainable_queue(void)
{
   static const char *payload =
       "{\"summary_status\":\"ok\",\"project\":\"\",\"chunks\":2,"
       "\"queue\":{\"pending\":65,\"running\":0,\"done\":0,\"failed\":0,\"total\":65},"
       "\"warnings\":[\"typed-fact extraction: 65 job(s) queued with nothing to drain "
       "them - no synthesis endpoint is configured\"],"
       "\"ingest_queue\":{\"pending\":0,\"running\":0,\"done_last_24h\":0}}";
   char out[2048] = "";
   capture_printer(pt_print_kb_status, "kb.status", payload, out, sizeof(out));

   /* The verdict is still ok -- that is the point, the warning cannot depend on
    * a degraded status to be shown. */
   assert(strstr(out, "ok") != NULL);
   assert(strstr(out, "65 pending") != NULL);
   assert(strstr(out, "WARNING") != NULL);
   assert(strstr(out, "nothing to drain") != NULL);
   printf("  kb status warns when the queue has nothing to drain it\n");
}

/* `config deploy-env` must be REACHABLE from the thin client.
 *
 * It was implemented in cmd_data.c and registered in that local subcommand table,
 * but had no /v1 route -- so on a managed deployment, where the operator's `aimee`
 * IS the thin client, the command its own doc comment recommends
 * (`eval "$(aimee config deploy-env)" && docker compose up -d`) answered
 * "'deploy-env' is not a subcommand of 'config'; try: show, get, set".
 *
 * That is not a cosmetic gap. Recreating a managed container without that env
 * drops every variable the compose file interpolates: EMBEDDER_MODEL, so the kb
 * refuses to serve, and AIMEE_KB_VARIANT, so ${AIMEE_KB_VARIANT:+-...} resolves
 * to the EMBEDDERLESS aimee-kb image -- silently, which is the exact outcome
 * config_emit_deploy_env's own comments were written to prevent.
 *
 * The help-coverage gate cannot catch this: it checks that routed commands are
 * documented, not that implemented ones are routed. */
static void test_config_deploy_env_is_routed(void)
{
   char buf[256];
   int n = cli_v1_subcommands("config", buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "deploy-env") != NULL);
   /* the neighbours stay routed */
   assert(strstr(buf, "show") != NULL);
   assert(strstr(buf, "get") != NULL);
   assert(strstr(buf, "set") != NULL);
   printf("  config deploy-env is routed to the thin client\n");
}

/* `memory get <id> --as-of <ts>` must reach the server, and its answer must be
 * printed.
 *
 * db2_memory_valid_at() had no production caller at all: the supersession WRITE
 * that closes valid_until was live, but nothing could ever READ the interval, so
 * "was this true on 12 June" was unanswerable for rows though the data was being
 * recorded. A DB2 primitive with only tests calling it is not a feature.
 *
 * The id must stay positional even with the flag present, and "unknown" has to
 * survive as a third answer -- the server returns it when it could not tell, and
 * folding that into "no" is how a bitemporal query lies. */
static void test_memory_get_as_of_is_wired(void)
{
   char *argv[] = {(char *)"42", (char *)"--as-of=2026-06-12 00:00:00"};
   cJSON *req = marshal_memory_get(2, argv);
   assert(req);
   cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
   assert(cJSON_IsNumber(id) && (int)id->valuedouble == 42);
   cJSON *as_of = cJSON_GetObjectItemCaseSensitive(req, "as_of");
   assert(cJSON_IsString(as_of));
   assert(strcmp(as_of->valuestring, "2026-06-12 00:00:00") == 0);
   cJSON_Delete(req);

   /* Without the flag the field is absent, not empty: the server only answers
    * the time question when it was asked. */
   char *plain[] = {(char *)"42"};
   req = marshal_memory_get(1, plain);
   assert(req);
   assert(cJSON_GetObjectItemCaseSensitive(req, "as_of") == NULL);
   cJSON_Delete(req);

   char out[1024] = "";
   capture_printer(pt_print_memory_get, "memory.get",
                   "{\"status\":\"ok\",\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"},"
                   "\"as_of\":\"2026-06-12 00:00:00\",\"valid_at\":false}",
                   out, sizeof(out));
   assert(strstr(out, "valid at 2026-06-12 00:00:00: no") != NULL);

   capture_printer(pt_print_memory_get, "memory.get",
                   "{\"status\":\"ok\",\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"},"
                   "\"as_of\":\"2026-06-12 00:00:00\",\"valid_at\":\"unknown\"}",
                   out, sizeof(out));
   assert(strstr(out, "valid at 2026-06-12 00:00:00: unknown") != NULL);

   /* A plain get prints no time line at all. */
   capture_printer(pt_print_memory_get, "memory.get",
                   "{\"status\":\"ok\",\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"}}",
                   out, sizeof(out));
   assert(strstr(out, "valid at") == NULL);

   printf("  memory get --as-of reaches the server and prints its answer\n");
}

/* `memory recall --query` must reach the field the server actually reads.
 *
 * It was marshalled as its own `query` key that nothing consumed:
 * handle_memory_recall reads task_hint, limit_tokens and session_start, and the
 * only other jo_str(req,"query") in the tree belongs to kb.search. So the flag
 * sent text, had it dropped, and fell back to the "session start" hint --
 * returning the recency bundle while looking like it had searched. Verified on a
 * deployment: a query matching three stored memories almost verbatim returned
 * none of them.
 *
 * An explicit --task must still win, so no existing invocation changes. */
static void test_memory_recall_query_feeds_the_hint(void)
{
   char *q[] = {(char *)"--query=nightly export manifest"};
   cJSON *req = marshal_memory_recall(1, q);
   assert(req);
   cJSON *hint = cJSON_GetObjectItemCaseSensitive(req, "task_hint");
   assert(cJSON_IsString(hint));
   assert(strcmp(hint->valuestring, "nightly export manifest") == 0);
   /* the dead key is gone, not merely ignored */
   assert(cJSON_GetObjectItemCaseSensitive(req, "query") == NULL);
   cJSON_Delete(req);

   /* --task wins over --query. */
   char *both[] = {(char *)"--task=explicit task", (char *)"--query=ignored"};
   req = marshal_memory_recall(2, both);
   assert(req);
   hint = cJSON_GetObjectItemCaseSensitive(req, "task_hint");
   assert(cJSON_IsString(hint) && strcmp(hint->valuestring, "explicit task") == 0);
   cJSON_Delete(req);

   /* Neither given: the session-start bundle, exactly as before. */
   req = marshal_memory_recall(0, NULL);
   assert(req);
   hint = cJSON_GetObjectItemCaseSensitive(req, "task_hint");
   assert(cJSON_IsString(hint) && strcmp(hint->valuestring, "session start") == 0);
   cJSON_Delete(req);

   printf("  memory recall --query feeds the hint the server reads\n");
}

int main(void)
{
   printf("test_cli_v1_subcommands\n");
   test_memory_store_keeps_unquoted_content();
   test_memory_get_as_of_is_wired();
   test_memory_recall_query_feeds_the_hint();
   test_kb_status_warns_about_undrainable_queue();
   test_config_deploy_env_is_routed();
   test_unknown_command_is_safe();
   test_known_command_lists_subcommands();
   test_list_formatting();
   test_small_buffer_does_not_overflow();
   test_bare_and_matchany_rows_excluded();
   test_kb_status_reports_backlog_and_degradation();
   test_agent_list_printer_shows_roles();
   test_agent_roles_printer_reports_roles();
   test_agent_roles_printer_read_is_not_reported_as_a_write();
   test_agent_personas_printer_reports_personas();
   test_run_failure_reports_dispatch_message();
   test_gated_method_refuses_without_confirmation();
   printf("test_cli_v1_subcommands: all passed\n");
   return 0;
}

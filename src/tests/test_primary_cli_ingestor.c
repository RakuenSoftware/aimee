/* test_primary_cli_ingestor.c -- the external-CLI-primary S2 seam (Slice 2):
 * the ingestor gate is default-off; enforce_preturn routes+binds a session
 * BEFORE send when given a resolved sid + an enforced-routed turn, is dial-gated,
 * and REFUSES to enforce (no-op, returns 0) on a missing session id. Real DB1 +
 * router + engine, stub executors. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "agent_shell.h"
#include "db1.h"
#include "primary_cli_ingestor.h"
#include "wfe_binding.h"
#include "wfe_engine.h"

/* --- stub agent_shell driver: records the open/send/recv/close call sequence,
 * captures whether the session was already S2-bound at send() time (proving
 * enforce-BEFORE-send), and emits a synthetic stream for the ingestor. --- */
static int g_open, g_send, g_recv, g_close;
static int g_bound_at_send;
static char g_check_sid[80];

static void *stub_open(const agent_shell_driver_t *d, const char *resume_id)
{
   (void)d;
   (void)resume_id;
   g_open++;
   return (void *)0x1; /* non-NULL opaque handle */
}
static int stub_send(void *h, const char *msg)
{
   (void)h;
   (void)msg;
   g_send++;
   char wi[80] = "";
   g_bound_at_send = (db1_wfe_binding_get(g_check_sid, wi, sizeof wi, NULL, 0) == 1 && wi[0]);
   return 0;
}
static int stub_recv(void *h, agent_shell_cb_t cb, void *user, volatile int *interrupted)
{
   (void)h;
   (void)interrupted;
   g_recv++;
   cb(SHELL_EVENT_TEXT_DELTA, "hello ", user);
   cb(SHELL_EVENT_TEXT_DELTA, "world", user);
   cb(SHELL_EVENT_TOOL_START, "Bash", user);
   cb(SHELL_EVENT_TOOL_COMPLETE, NULL, user);
   cb(SHELL_EVENT_SESSION_ID, "claude-sess-1", user);
   cb(SHELL_EVENT_TURN_COMPLETE, NULL, user);
   return 0;
}
static void stub_close(void *h)
{
   (void)h;
   g_close++;
}
static const agent_shell_driver_t STUB_DRIVER = {.name = "stubcli",
                                                 .open = stub_open,
                                                 .send = stub_send,
                                                 .recv = stub_recv,
                                                 .close = stub_close};

/* Link shims: agent_shell.o's agent_shell_drivers_init() references these built-in
 * drivers (defined in the heavy per-CLI driver objects). This test never calls
 * drivers_init -- it registers only STUB_DRIVER -- so zero-initialized stand-ins
 * satisfy the linker without pulling in the real fork/exec CLI machinery. */
agent_shell_driver_t claude_shell_driver;
agent_shell_driver_t claude_pty_shell_driver;
agent_shell_driver_t codex_shell_driver;
agent_shell_driver_t gemini_shell_driver;

/* enforced managed workflow "mc" (valid manager shape w/ terminal gate.deliver). */
static const char *WF_MC = "name: mc\n"
                           "enforced: true\n"
                           "start: understand\n"
                           "nodes:\n"
                           "  - id: understand\n"
                           "    block: understand\n"
                           "    next: split\n"
                           "  - id: split\n"
                           "    block: split\n"
                           "    in:\n"
                           "      intent: understand.out\n"
                           "    next: implement\n"
                           "  - id: implement\n"
                           "    block: implement\n"
                           "    in:\n"
                           "      plan: split.out\n"
                           "    next: freeze\n"
                           "  - id: freeze\n"
                           "    block: freeze\n"
                           "    in:\n"
                           "      branch: implement.out\n"
                           "    next: review\n"
                           "  - id: review\n"
                           "    block: review\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    params:\n"
                           "      reviewer: contrarian\n"
                           "    on_pass: rt\n"
                           "    on_fail: split\n"
                           "  - id: rt\n"
                           "    block: gate.roundtable\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    params:\n"
                           "      panel:\n"
                           "        required:\n"
                           "          - security\n"
                           "          - architect\n"
                           "    on_pass: deliver\n"
                           "    on_fail: split\n"
                           "  - id: deliver\n"
                           "    block: gate.deliver\n"
                           "    in:\n"
                           "      verdict: rt.out\n";

static void setup_home(void)
{
   char tmpl[] = "/tmp/pci_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[640];
   snprintf(path, sizeof path, "%s/mc.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF_MC, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

static int bound(const char *sid)
{
   char wi[80] = "";
   return db1_wfe_binding_get(sid, wi, sizeof wi, NULL, 0) == 1 && wi[0];
}

int main(void)
{
   printf("primary-cli-ingestor: ");

   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_reset_block_executors();
   wfe_register_stub_executors();

   /* the gate is default-off and only true for {1,on,true} */
   unsetenv("AIMEE_PRIMARY_CLI_INGESTOR");
   assert(primary_cli_ingestor_enabled() == 0);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "1", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "on", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "true", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "0", 1);
   assert(primary_cli_ingestor_enabled() == 0);

   const char *SID = "1a2b3c4d";

   /* dial OFF -> enforce_preturn is inert even with a real sid + enforced route */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(primary_cli_ingestor_enforce_preturn(SID, "use mc fix the bug", NULL) == 0);
   assert(!bound(SID));

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "advisory", 1);

   /* trust boundary: no resolvable sid -> no-op (0), NEVER a silent bind */
   assert(primary_cli_ingestor_enforce_preturn("", "use mc fix the bug", NULL) == 0);
   assert(primary_cli_ingestor_enforce_preturn(NULL, "use mc fix the bug", NULL) == 0);

   /* an enforced-routed turn WITH a sid -> binds (preventive, before send) */
   assert(primary_cli_ingestor_enforce_preturn(SID, "use mc fix the bug", NULL) == 1);
   assert(bound(SID));

   /* a non-enforced (converse) turn -> stays unbound */
   const char *SID2 = "beefcafe";
   assert(primary_cli_ingestor_enforce_preturn(SID2, "hello there", NULL) == 0);
   assert(!bound(SID2));

   /* Slice 3: the turn orchestrator drives the agent_shell backend + ingests its
    * stream, enforcing BEFORE send. */
   agent_shell_driver_register(&STUB_DRIVER);
   const char *SID3 = "cafed00d";
   snprintf(g_check_sid, sizeof g_check_sid, "%s", SID3);
   g_open = g_send = g_recv = g_close = g_bound_at_send = 0;

   primary_cli_turn_result_t r;
   int rc = primary_cli_ingestor_turn(SID3, "use mc do the thing", NULL, "stubcli", NULL, &r, NULL);
   assert(rc == 0);
   assert(g_open == 1 && g_send == 1 && g_recv == 1 && g_close == 1); /* full lifecycle */
   assert(r.bound == 1);                                 /* enforced-routed turn is managed */
   assert(g_bound_at_send == 1);                         /* ENFORCE RAN BEFORE SEND (preventive) */
   assert(r.text && strcmp(r.text, "hello world") == 0); /* text deltas ingested */
   assert(r.session && strcmp(r.session, "claude-sess-1") == 0); /* backend sid captured */
   assert(r.tool_calls == 1);                                    /* native tool observed (audit) */
   assert(!r.error[0]);
   primary_cli_turn_result_free(&r);

   /* a converse turn still runs but is NOT bound (unmanaged, not pretend-enforced) */
   const char *SID4 = "0ddba11";
   snprintf(g_check_sid, sizeof g_check_sid, "%s", SID4);
   g_bound_at_send = 1; /* poison: must be reset to 0 by the check */
   primary_cli_turn_result_t r2;
   assert(primary_cli_ingestor_turn(SID4, "hello there", NULL, "stubcli", NULL, &r2, NULL) == 0);
   assert(r2.bound == 0 && g_bound_at_send == 0); /* not bound at send */
   assert(r2.text && strcmp(r2.text, "hello world") == 0);
   primary_cli_turn_result_free(&r2);

   /* an unknown driver -> clean error, no crash, no result leak */
   primary_cli_turn_result_t r3;
   assert(primary_cli_ingestor_turn(SID3, "hi", NULL, "no-such-driver", NULL, &r3, NULL) == -1);
   assert(r3.error[0] && !r3.text && !r3.session);
   primary_cli_turn_result_free(&r3);

   printf("ok\n");
   return 0;
}

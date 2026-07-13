/* test_wfe_roundtable.c -- W5: the fail-closed gate decision rule + the
 * gate.roundtable executor through the engine with a mock panel provider. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_roundtable.h"
#include "wfe_verdict.h"

static wfe_verdict_t mk(const char *persona, wfe_verdict_kind_t k, const char *hash, int hs)
{
   wfe_verdict_t v;
   memset(&v, 0, sizeof v);
   snprintf(v.persona, sizeof v.persona, "%s", persona);
   v.schema_version = WFE_VERDICT_SCHEMA;
   snprintf(v.reviewed_content_hash, sizeof v.reviewed_content_hash, "%s", hash ? hash : "");
   v.kind = k;
   v.high_sev_blockers = hs;
   return v;
}

/* ---- mock panel provider for the engine sub-test ---- */
static int g_mode; /* 0=approve all required, 1=request changes, 2=unreachable */
/* last review packet the panel saw (S1: proposal + focus reach the panel) */
static char g_seen_proposal[512];
static char g_seen_focus[128];
static char g_seen_workdir[512];
static int mock_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)eligible;
   (void)nelig;
   snprintf(g_seen_proposal, sizeof g_seen_proposal, "%s", pkt->proposal);
   snprintf(g_seen_focus, sizeof g_seen_focus, "%s", pkt->focus);
   snprintf(g_seen_workdir, sizeof g_seen_workdir, "%s", pkt->workdir);
   if (g_mode == 2)
      return -1;
   int n = 0;
   for (int i = 0; i < nreq && n < max; i++)
      out[n++] = mk(required[i], g_mode == 0 ? WFE_V_APPROVE : WFE_V_REQUEST_CHANGES,
                    pkt->artifact_hash, 0);
   return n;
}

static const char *RT = "name: rt\n"
                        "start: draft\n"
                        "nodes:\n"
                        "  - id: draft\n"
                        "    block: author.proposal\n"
                        "    next: gate\n"
                        "  - id: gate\n"
                        "    block: gate.roundtable\n"
                        "    in:\n"
                        "      src: draft.out\n"
                        "    params:\n"
                        "      panel:\n"
                        "        required:\n"
                        "          - security\n"
                        "          - architect\n"
                        "      quorum: 2\n"
                        "      focus: completion and missing tests\n"
                        "    on_pass: pr\n"
                        "    on_fail: draft\n"
                        "  - id: pr\n"
                        "    block: pr.open\n"
                        "    in:\n"
                        "      src: draft.out\n"
                        "    next: done\n"
                        "  - id: done\n"
                        "    block: merge\n"
                        "    in:\n"
                        "      pr: pr.out\n";

static void setup_home(void)
{
   char d[] = "/tmp/wfe_rt_XXXXXX";
   char *dir = wfe_test_mkdtemp(d);
   assert(dir);
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char p[200];
   snprintf(p, sizeof p, "%s/rt.yaml", wf);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(RT, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
}

int main(void)
{
   printf("wfe-roundtable: ");
   const char *req2[] = {"security", "architect"};

   /* --- effective quorum (single-lens floor) --- */
   assert(wfe_gate_effective_quorum(0, 4) == 4);
   assert(wfe_gate_effective_quorum(1, 1) == 2); /* floor of 2 */
   assert(wfe_gate_effective_quorum(3, 2) == 3);
   assert(wfe_gate_effective_quorum(5, 4) == 5);

   /* --- decision matrix (pure) --- */
   {
      char rs[160];
      /* both approve, quorum 2, no high-sev -> APPROVE */
      wfe_verdict_t a[] = {mk("security", WFE_V_APPROVE, "H", 0),
                           mk("architect", WFE_V_APPROVE, "H", 0)};
      assert(wfe_gate_decide(a, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_APPROVE);

      /* missing a required persona -> DEGRADED */
      wfe_verdict_t b[] = {mk("security", WFE_V_APPROVE, "H", 0)};
      assert(wfe_gate_decide(b, 1, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_DEGRADED);

      /* high-sev blocker present -> CHANGES even if quorum met */
      wfe_verdict_t c[] = {mk("security", WFE_V_APPROVE, "H", 0),
                           mk("architect", WFE_V_APPROVE, "H", 1)};
      assert(wfe_gate_decide(c, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_CHANGES);

      /* COMMENT is non-blocking: approve + comment meets quorum 2 -> APPROVE */
      wfe_verdict_t d[] = {mk("security", WFE_V_APPROVE, "H", 0),
                           mk("architect", WFE_V_COMMENT, "H", 0)};
      assert(wfe_gate_decide(d, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_APPROVE);

      /* ...but comments alone never pass: at least one explicit approve. */
      wfe_verdict_t d2[] = {mk("security", WFE_V_COMMENT, "H", 0),
                            mk("architect", WFE_V_COMMENT, "H", 0)};
      assert(wfe_gate_decide(d2, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_CHANGES);

      /* ...and ANY request_changes loops, even with quorum-many non-blocking. */
      wfe_verdict_t d3[] = {mk("security", WFE_V_APPROVE, "H", 0),
                            mk("architect", WFE_V_COMMENT, "H", 0),
                            mk("qa", WFE_V_REQUEST_CHANGES, "H", 0)};
      assert(wfe_gate_decide(d3, 3, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_CHANGES);

      /* tampered hash on a REQUIRED persona: it never validly reviewed THIS
       * artifact -> integrity failure -> DEGRADED (not a definitive CHANGES). */
      wfe_verdict_t e[] = {mk("security", WFE_V_APPROVE, "WRONG", 0),
                           mk("architect", WFE_V_APPROVE, "H", 0)};
      assert(wfe_gate_decide(e, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_DEGRADED);

      /* malformed REQUIRED persona -> integrity failure -> DEGRADED */
      wfe_verdict_t g[] = {mk("security", WFE_V_APPROVE, "H", 0),
                           mk("architect", WFE_V_MALFORMED, "H", 0)};
      assert(wfe_gate_decide(g, 2, req2, 2, 2, "H", rs, sizeof rs) == WFE_GATE_DEGRADED);

      /* REGRESSION: an untrustworthy REQUIRED verdict must NOT be papered over by
       * other panelists meeting quorum. required=[security] returns malformed
       * while architect+qa validly approve (quorum 2 met): the required lens never
       * reviewed the artifact, so the gate DEGRADES rather than APPROVES. */
      const char *req1[] = {"security"};
      wfe_verdict_t h[] = {mk("security", WFE_V_MALFORMED, "H", 0),
                           mk("architect", WFE_V_APPROVE, "H", 0), mk("qa", WFE_V_APPROVE, "H", 0)};
      assert(wfe_gate_decide(h, 3, req1, 1, 2, "H", rs, sizeof rs) == WFE_GATE_DEGRADED);
   }

   /* --- engine integration via mock provider --- */
   setup_home();
   assert(db1_init(":memory:") == 0);

   /* mode 0: panel approves -> gate advances -> accepted. S1: the panel must see
    * the originating proposal text AND the focus lens from params.focus. */
   {
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_roundtable_gate();
      g_mode = 0;
      wfe_set_panel_provider(mock_panel);
      /* a real proposal file so read_text_capped returns its content to the panel */
      char pp[] = "/tmp/wfe_rt_proposal_XXXXXX";
      int fd = mkstemp(pp);
      assert(fd >= 0);
      const char *PTEXT = "PROPOSAL: add a widget with tests.";
      assert(write(fd, PTEXT, strlen(PTEXT)) == (ssize_t)strlen(PTEXT));
      close(fd);
      g_seen_proposal[0] = g_seen_focus[0] = g_seen_workdir[0] = '\0';
      /* No per-item worktree is set on this run; the panel must still get a workdir to
       * review in — the shared repo (AIMEE_WORKFLOW_REPO) — never an empty string that
       * would make a live panel park panel_unreachable (the .253 live-run regression). */
      setenv("AIMEE_WORKFLOW_REPO", "/tmp", 1);
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rt", "r0", pp, "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
      assert(strcmp(g_seen_proposal, PTEXT) == 0); /* proposal reached the panel */
      assert(strcmp(g_seen_focus, "completion and missing tests") == 0); /* focus lens too */
      assert(strcmp(g_seen_workdir, "/tmp") == 0); /* worktree empty -> repo-dir fallback */
      unsetenv("AIMEE_WORKFLOW_REPO");
      unlink(pp);
   }

   /* mode 1: panel requests changes -> gate loops -> max_attempts -> pending_human */
   {
      g_mode = 1;
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rt", "r1", "p1", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   /* default (live §0) provider: cannot compose -> DEGRADED -> pending */
   {
      wfe_set_panel_provider(NULL);
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rt", "r2", "p2", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strncmp(wi.pause_reason, "panel_", 6) == 0); /* panel_unreachable */
   }

   printf("ok\n");
   return 0;
}

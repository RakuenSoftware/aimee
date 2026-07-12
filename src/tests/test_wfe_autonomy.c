/* test_wfe_autonomy.c -- W6: the autonomy driver + human-only gate-override. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_approval.h"
#include "wfe_autonomy.h"
#include "wfe_blocks.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_roundtable.h"

/* plain human gate — inviolable: autonomous mode parks at it, never auto-satisfies */
static const char *AUTO = "name: auto\n"
                          "start: draft\n"
                          "nodes:\n"
                          "  - id: draft\n"
                          "    block: author.proposal\n"
                          "    next: approve\n"
                          "  - id: approve\n"
                          "    block: gate.human\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    next: pr\n"
                          "  - id: pr\n"
                          "    block: pr.open\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    next: done\n"
                          "  - id: done\n"
                          "    block: merge\n"
                          "    in:\n"
                          "      pr: pr.out\n";

/* roundtable gate (left to the live §0 provider -> degraded) */
static const char *RT = "name: rta\n"
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

/* This stub-executor test does not link wfe_blocks.o (it uses register_stub); the
 * autonomy driver's terminal cleanup calls wfe_worktree_cleanup, so provide a no-op
 * (the real helper is exercised in test_wfe_delegate_seam over a git fixture). */
int wfe_worktree_cleanup(const char *worktree, const char *repo_local)
{
   (void)worktree;
   (void)repo_local;
   return 0;
}

/* A mock panel provider for the auto-retry tests. It DEGRADES its first
 * g_stub_degrade_calls convenings (returning 0 verdicts so a required persona is
 * unfilled -> the gate scores DEGRADED -> panel_degraded; or WFE_PANEL_UNREACHABLE
 * when g_stub_return_unreachable is set), then APPROVES: one APPROVE verdict per
 * required persona, echoing the packet's artifact_hash so the gate's hash-identity
 * check passes. g_stub_panel_calls counts convenings — the retry tests assert on
 * it to prove that clearing the pause actually RE-RUNS the roundtable node. */
#define ALWAYS_DEGRADE 1000 /* g_stub_degrade_calls sentinel: degrade every convening */
static int g_stub_panel_calls;
static int g_stub_degrade_calls; /* degrade the first N convenings (ALWAYS_DEGRADE = every one) */
static int g_stub_return_unreachable;
static int stub_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   /* The verdict set is sized from `required` (the lenses), not the eligible-agent
    * pool, so the mock ignores eligible/nelig — matches how the gate scores. */
   (void)eligible;
   (void)nelig;
   if (!pkt)
      return 0; /* defensive: the real seam never passes NULL, fail closed to DEGRADED */
   g_stub_panel_calls++;
   if (g_stub_panel_calls <= g_stub_degrade_calls)
      return g_stub_return_unreachable ? WFE_PANEL_UNREACHABLE : 0;
   int n = 0;
   for (int i = 0; i < nreq && n < max; i++)
   {
      wfe_verdict_t *v = &out[n++];
      memset(v, 0, sizeof *v);
      snprintf(v->persona, sizeof v->persona, "%s", required[i]);
      v->schema_version = WFE_VERDICT_SCHEMA;
      snprintf(v->reviewed_content_hash, sizeof v->reviewed_content_hash, "%s",
               pkt->artifact_hash ? pkt->artifact_hash : "");
      v->kind = WFE_V_APPROVE;
      v->high_sev_blockers = 0;
   }
   return n;
}

static void write_wf(const char *dir, const char *name, const char *body)
{
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/%s.yaml", dir, name);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(body, f);
   fclose(f);
}

int main(void)
{
   printf("wfe-autonomy: ");
   char d[] = "/tmp/wfe_auto_XXXXXX";
   char *dir = wfe_test_mkdtemp(d);
   assert(dir);
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   write_wf(dir, "auto", AUTO);
   write_wf(dir, "rta", RT);
   setenv("AIMEE_HOME", dir, 1);
   assert(db1_init(":memory:") == 0);
   assert(wfe_approval_ensure_key() == 0);

   wfe_reset_block_executors();
   wfe_register_stub_executors();
   wfe_register_human_gate();
   wfe_register_roundtable_gate();
   wfe_set_panel_provider(NULL); /* live §0 -> degraded */

   /* A1: autonomous + human gate -> PARKS (inviolable; never auto-satisfied). */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a1", "a1", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   /* A2: interactive + same gate -> parks pending_human (no auto-approval) */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a2", "a2", "interactive", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   /* A3: autonomous + roundtable (live §0) -> parks; never self-approves */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a3", "a3", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strncmp(wi.pause_reason, "panel_", 6) == 0);
   }

   /* A4: gate-override resumes a parked item; cap forces rejected */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a4", "a4", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0); /* parks at human gate */
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
      assert(wfe_gate_override(id, "approve", "alice", "ship it", err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
   }

   /* A5: override cap -> forced rejected on the (max+1)th */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a5", "a5", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0); /* park at the human gate "approve" */
      int forced = 0;
      for (int k = 0; k < WFE_MAX_OVERRIDES + 1; k++)
      {
         /* each override clears the pause; re-park (as if the gate re-fired after
          * a stale approval) so the next override is on a parked item. */
         db1_work_item_set_pause(id, "pending_human", "approve");
         int rc = wfe_gate_override(id, "approve", "alice", "x", err, sizeof err);
         if (rc == 1)
            forced = 1;
      }
      assert(forced);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "rejected") == 0);
   }

   /* A6: autonomous merge-target rail (WP-5) — protected branches are refused;
    * the configured base must be non-protected for pr.open/merge to proceed. */
   {
      assert(wfe_base_is_protected("main"));
      assert(wfe_base_is_protected("master"));
      assert(wfe_base_is_protected("Main"));   /* case-insensitive */
      assert(wfe_base_is_protected("MASTER")); /* case-insensitive */
      assert(wfe_base_is_protected("release-1.2"));
      assert(wfe_base_is_protected("release/v2.0"));
      assert(wfe_base_is_protected("")); /* empty -> protected (fail closed) */
      assert(!wfe_base_is_protected("testing"));
      assert(!wfe_base_is_protected("aimee/wi/abc"));
      assert(!wfe_base_is_protected("release-notes-edit")); /* not the release train */
      unsetenv("AIMEE_AUTONOMY_BASE");
      assert(strcmp(wfe_autonomous_base(), "testing") == 0);
      assert(wfe_autonomous_target_ok());
      setenv("AIMEE_AUTONOMY_BASE", "main", 1); /* misconfig -> guard refuses */
      assert(!wfe_autonomous_target_ok());
      setenv("AIMEE_AUTONOMY_BASE", "dev", 1);
      assert(wfe_autonomous_target_ok());
      unsetenv("AIMEE_AUTONOMY_BASE");
   }

   /* A7: per-run turn cap (WP-5) — once the cumulative audit-event count reaches
    * the cap, an autonomous run parks turn_cap_exceeded BEFORE advancing further
    * (so a runaway loop can't burn unbounded turns). The pause reason names the
    * ACTUAL breach (the turn cap), not the unrelated dollar cost cap. */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "cap1", "cap1", "autonomous", id, err, sizeof err) == 0);
      for (int k = 0; k < 5; k++)
         db1_lifecycle_event_add(id, "draft", "test", "t", "pad", "", 0);
      setenv("AIMEE_AUTONOMY_MAX_TURNS", "3", 1);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      unsetenv("AIMEE_AUTONOMY_MAX_TURNS");
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "turn_cap_exceeded") == 0);
   }

   /* A8: server-side cost estimate (WP-5) — wall-clock seconds * rate; negative
    * elapsed clamps to 0; the rate is env-overridable. */
   {
      unsetenv("AIMEE_AUTONOMY_USD_PER_SEC");
      assert(wfe_autonomy_cost_estimate(0.0) == 0.0);
      assert(wfe_autonomy_cost_estimate(-5.0) == 0.0); /* clamp */
      double c = wfe_autonomy_cost_estimate(10.0);     /* 10s * 0.0005 = 0.005 */
      assert(c > 0.0049 && c < 0.0051);
      setenv("AIMEE_AUTONOMY_USD_PER_SEC", "0.01", 1);
      c = wfe_autonomy_cost_estimate(10.0); /* 10s * 0.01 = 0.1 */
      assert(c > 0.099 && c < 0.101);
      setenv("AIMEE_AUTONOMY_USD_PER_SEC", "junk", 1); /* malformed -> default */
      c = wfe_autonomy_cost_estimate(10.0);
      assert(c > 0.0049 && c < 0.0051);
      unsetenv("AIMEE_AUTONOMY_USD_PER_SEC");
   }

   /* A9: an UNRECOVERABLE advance failure (current_stage is not a node in the
    * workflow — e.g. an orphaned stage after a rename, or the same class as a
    * looped node with no on_fail edge) parks the item 'stuck' instead of the
    * scheduler retry-failing it forever while the UI shows "running". A re-sweep
    * is a no-op: still stuck, no new lifecycle event (no spam). */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "stuck1", "stuck1", "autonomous", id, err, sizeof err) ==
             0);
      /* orphan the stage: "ghoststage" is not a node in the "auto" workflow. */
      assert(db1_work_item_set_stage(id, "ghoststage", "") == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "stuck") == 0);

      db1_lifecycle_event_t *evs = NULL;
      int before = db1_lifecycle_event_list(id, &evs);
      free(evs);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* re-sweep: early-out */
      evs = NULL;
      int after = db1_lifecycle_event_list(id, &evs);
      free(evs);
      assert(after == before); /* no re-park spam */
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "stuck") == 0);
   }

   /* PC2: the CI-event webhook routes by pr_ref -> work-item id. */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "pc2repo", "pc2prop", "autonomous", id, err,
                                  sizeof err) == 0);
      assert(db1_work_item_set_pr_ref(id, "https://github.com/o/r/pull/77") == 0);
      char got[80] = "";
      assert(db1_work_item_id_by_pr_ref("https://github.com/o/r/pull/77", got, sizeof got) == 1);
      assert(strcmp(got, id) == 0);
      /* an unknown pr_ref resolves to none (webhook returns 404) */
      char none[80] = "x";
      assert(db1_work_item_id_by_pr_ref("https://github.com/o/r/pull/999", none, sizeof none) == 0);
      assert(none[0] == '\0');
      /* empty / NULL pr_ref is a bad arg */
      assert(db1_work_item_id_by_pr_ref("", got, sizeof got) == -1);
      assert(db1_work_item_id_by_pr_ref(NULL, got, sizeof got) == -1);
   }

   /* A10: a TRANSIENT roundtable park (panel_degraded) is AUTO-RETRIED by the
    * autonomy driver — clearing the pause re-runs the roundtable node — and a
    * recovered panel self-heals PAST the gate instead of dead-ending forever. */
   {
      g_stub_panel_calls = 0;
      g_stub_degrade_calls = 2; /* degrade convenings 1-2, approve #3 */
      g_stub_return_unreachable = 0;
      wfe_set_panel_provider(stub_panel);
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "5", 1);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a10", "a10", "autonomous", id, err, sizeof err) == 0);
      db1_work_item_t wi;
      /* run 1: initial convene -> degraded -> parks panel_degraded (no retry yet). */
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);
      assert(g_stub_panel_calls == 1);
      /* run 2: retry #1 -> convene #2 -> still degraded -> re-parks. */
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);
      assert(g_stub_panel_calls == 2);
      /* run 3: retry #2 -> convene #3 -> APPROVE -> advances past the gate. The
       * convene count reaching 3 proves the pause-clear actually RE-RAN the node. */
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(g_stub_panel_calls == 3);
      assert(db1_work_item_get(id, &wi) == 1);
      /* Recovered: not parked for ANY reason, and advanced OFF the roundtable gate
       * (a stronger check than "not a panel_ reason" — that also passed for an
       * unrelated runaway park like turn_cap_exceeded). */
      assert(wi.pause_reason[0] == '\0');
      assert(strcmp(wi.current_stage, "gate") != 0);

      unsetenv("AIMEE_AUTONOMY_PANEL_RETRIES");
      wfe_set_panel_provider(NULL);
   }

   /* A11: a persistently-degraded panel is retried only up to the cap, then stays
    * parked to escalate to a human — exactly `cap` panel_retry events are recorded
    * (per (work item, stage)), and no further convenings happen once spent. */
   {
      g_stub_panel_calls = 0;
      g_stub_degrade_calls = ALWAYS_DEGRADE; /* always degrade */
      g_stub_return_unreachable = 0;
      wfe_set_panel_provider(stub_panel);
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "2", 1);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a11", "a11", "autonomous", id, err, sizeof err) == 0);
      for (int k = 0; k < 5; k++) /* 1 initial + 2 retries convene; the rest early-out */
         assert(wfe_autonomy_run(id, err, sizeof err) == 0);

      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);
      /* the retry key is stable: every degraded re-park lands on the same node id,
       * so the per-stage budget does not drift across retries. */
      assert(strcmp(wi.current_stage, "gate") == 0);
      assert(g_stub_panel_calls == 3); /* 1 initial + 2 retries, then capped */

      db1_lifecycle_event_t *evs = NULL;
      int n = db1_lifecycle_event_list(id, &evs);
      int retries = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(evs[i].kind, "panel_retry") == 0 && strcmp(evs[i].stage, "gate") == 0)
            retries++;
      free(evs);
      assert(retries == 2); /* exactly the cap; a failed clear would NOT count here */

      unsetenv("AIMEE_AUTONOMY_PANEL_RETRIES");
      wfe_set_panel_provider(NULL);
   }

   /* A12: panel_unreachable (the panel could not be reached at all) is the same
    * transient class and is auto-retried too, not escalated immediately. */
   {
      g_stub_panel_calls = 0;
      g_stub_degrade_calls = ALWAYS_DEGRADE;
      g_stub_return_unreachable = 1; /* provider returns WFE_PANEL_UNREACHABLE */
      wfe_set_panel_provider(stub_panel);
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "3", 1);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a12", "a12", "autonomous", id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* initial -> panel_unreachable */
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_unreachable") == 0);
      assert(g_stub_panel_calls == 1);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* retry #1 -> re-convene */
      assert(g_stub_panel_calls == 2); /* proves re-run for the unreachable class too */

      db1_lifecycle_event_t *evs = NULL;
      int n = db1_lifecycle_event_list(id, &evs);
      int retries = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(evs[i].kind, "panel_retry") == 0)
            retries++;
      free(evs);
      assert(retries == 1);

      unsetenv("AIMEE_AUTONOMY_PANEL_RETRIES");
      wfe_set_panel_provider(NULL);
   }

   /* A13: AIMEE_AUTONOMY_PANEL_RETRIES=0 is the operator escape hatch — auto-retry
    * is disabled and a transient park escalates to a human immediately (the
    * pre-feature behavior), with the panel never re-convened. */
   {
      g_stub_panel_calls = 0;
      g_stub_degrade_calls = ALWAYS_DEGRADE;
      g_stub_return_unreachable = 0;
      wfe_set_panel_provider(stub_panel);
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "0", 1);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a13", "a13", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* initial -> parks panel_degraded */
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);
      assert(g_stub_panel_calls == 1);
      /* subsequent sweeps do NOT retry: no re-convene, stays parked for a human. */
      for (int k = 0; k < 3; k++)
         assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(g_stub_panel_calls == 1);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);

      db1_lifecycle_event_t *evs = NULL;
      int n = db1_lifecycle_event_list(id, &evs);
      int retries = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(evs[i].kind, "panel_retry") == 0)
            retries++;
      free(evs);
      assert(retries == 0); /* disabled -> no retry events at all */

      unsetenv("AIMEE_AUTONOMY_PANEL_RETRIES");
      wfe_set_panel_provider(NULL);
   }

   /* A14: a malformed AIMEE_AUTONOMY_PANEL_RETRIES falls back to the DEFAULT cap —
    * retry stays enabled (a typo must not silently disable the rail like 0 does,
    * nor crash the parser) AND is still bounded at the default. Driving enough
    * sweeps pins the default EXACTLY: initial convene + default retries, no more —
    * distinguishing it from a silent collapse to 1 or an uncapped loop. */
   {
      g_stub_panel_calls = 0;
      g_stub_degrade_calls = ALWAYS_DEGRADE;
      g_stub_return_unreachable = 0;
      wfe_set_panel_provider(stub_panel);
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "not-a-number", 1);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a14", "a14", "autonomous", id, err, sizeof err) == 0);
      for (int k = 0; k < WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT + 3; k++)
         assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(g_stub_panel_calls == WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT + 1);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "panel_degraded") == 0);

      /* negative value is also treated as malformed -> default (retry stays
       * enabled, NOT disabled like an explicit 0): a fresh item still retries. */
      g_stub_panel_calls = 0;
      setenv("AIMEE_AUTONOMY_PANEL_RETRIES", "-3", 1);
      char idn[80] = "";
      assert(wfe_work_item_create("rta", "a14n", "a14n", "autonomous", idn, err, sizeof err) == 0);
      assert(wfe_autonomy_run(idn, err, sizeof err) == 0); /* initial park */
      assert(wfe_autonomy_run(idn, err, sizeof err) == 0); /* retry #1 -> not disabled */
      assert(g_stub_panel_calls == 2);

      unsetenv("AIMEE_AUTONOMY_PANEL_RETRIES");
      wfe_set_panel_provider(NULL);
   }

   /* A15: a NON-transient park (pending_human) is left UNTOUCHED by the retry path.
    * It is neither short-circuited early nor retried — it falls through to the
    * advance loop, which re-parks it without re-running work. This pins the
    * invariant H1 depends on: adding the transient-retry branch did not change
    * behavior for any other park class. */
   {
      g_stub_panel_calls = 0;
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a15", "a15", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* parks pending_human */
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
      char stage0[64];
      snprintf(stage0, sizeof stage0, "%s", wi.current_stage);

      /* a second sweep must NOT touch it: same reason, same stage, no panel activity. */
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
      assert(strcmp(wi.current_stage, stage0) == 0);
      assert(g_stub_panel_calls == 0);

      db1_lifecycle_event_t *evs = NULL;
      int n = db1_lifecycle_event_list(id, &evs);
      int retries = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(evs[i].kind, "panel_retry") == 0)
            retries++;
      free(evs);
      assert(retries == 0); /* the transient-retry branch never fired for pending_human */
   }

   printf("ok\n");
   return 0;
}

/* wfe_live_panel.c -- the live roundtable panel provider.
 *
 * gate.roundtable calls this to convene a diverse panel: it dispatches ONE read-only
 * review delegate per REQUIRED persona (the same synchronous, worktree-reset-enforced
 * mechanism the implement adversarial judge uses), then maps each reply to a verdict
 * (wfe_panel_verdict_from_review). A persona that cannot be dispatched is left WITHOUT
 * a verdict so wfe_gate_decide DEGRADES (parks) rather than approving on a partial
 * panel — fail closed. Registered from wfe_autonomy_register.
 *
 * NOTE: the per-persona dispatch requires reachable review agents, so it is exercised
 * by integration (a live deployment), not the unit suite; the risk-bearing mapping is
 * unit-tested in test_wfe_panel_verdict. The panel is inert until a run reaches a
 * gate.roundtable node, so registration alone changes nothing.
 */
#include "aimee.h"

#include "wfe_panel_verdict.h"
#include "wfe_roundtable.h"

#include "agent.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "delegate_role.h"
#include "persona.h"
#include "provider_catalog.h"
#include "roundtable_preset.h"
#include "roundtable_seat_resolve.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run `git -C workdir <args...>`, discard output. */
static void panel_git(const char *workdir, const char *const extra[], int extra_n)
{
   const char *argv[16];
   int argc = 0;
   argv[argc++] = "git";
   argv[argc++] = "-C";
   argv[argc++] = workdir;
   for (int i = 0; i < extra_n && argc < 15; i++)
      argv[argc++] = extra[i];
   argv[argc] = NULL;
   char *o = NULL;
   (void)safe_exec_capture(argv, &o, 1 << 14);
   free(o);
}

/* Max panel seats (matches the verdict-array bound the gate.roundtable executor
 * passes as `max`). */
#define WFE_PANEL_MAX 16
/* Per-round wall-clock ceiling for the parallel panel: a panelist still running
 * at the deadline is abandoned (its lens left unfilled) so one hung model can
 * never wedge the round. */
#define WFE_PANEL_DEADLINE_MS 300000

/* How long an unseatable $random lens QUEUES for a review agent before the
 * gate degrades. The review roster is small and shared with implement
 * delegates: under a parallel fleet, "no eligible review agent right now" is
 * usually transient (provider health streak, seats busy) — waiting out the
 * contention converts an instant panel_degraded park into a completed panel.
 * 0 disables queueing (legacy instant-degrade). */
static long wfe_panel_seat_wait_secs(void)
{
   const char *v = getenv("AIMEE_PANEL_SEAT_WAIT_SECS");
   if (v && v[0])
   {
      char *end = NULL;
      long s = strtol(v, &end, 10);
      if (end && *end == '\0' && s >= 0 && s <= 3600)
         return s;
   }
   return 300;
}

#define WFE_PANEL_SEAT_POLL_SECS 15

/* Build the per-persona review prompt. The change under review is embedded directly
 * as a diff so the panelist reads the delta and navigates surrounding code via the
 * aimee index (find_symbol/search_memory) instead of re-running git and sweeping the
 * worktree. Returns a malloc'd prompt sized to fit the diff (caller frees), or NULL
 * on allocation failure. */
static char *build_prompt(const char *persona, const wfe_review_packet_t *pkt)
{
   const char *focus =
       (pkt->focus && pkt->focus[0]) ? pkt->focus : "correctness, quality, and completeness";
   const char *proposal = (pkt->proposal && pkt->proposal[0]) ? pkt->proposal : "(none provided)";
   const char *diff = (pkt->diff && pkt->diff[0])
                          ? pkt->diff
                          : "(no code diff — review the plan/proposal artifact against the ask)";
   size_t cap = 2048 + strlen(focus) + strlen(proposal) + strlen(diff);
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   snprintf(buf, cap,
            "You are the %s lens on a review roundtable. Review the CHANGE UNDER REVIEW below "
            "AGAINST what was asked. Do NOT edit files, and do NOT sweep or re-read the repo to "
            "reconstruct context: trust aimee's index/graph (find_symbol, search_memory) as the "
            "authoritative source for the current codebase, and open a file only to confirm what "
            "the index cannot resolve.\n\nFOCUS: %s\n\nORIGINAL PROPOSAL/REQUEST:\n%.4000s\n\n"
            "CHANGE UNDER REVIEW (diff vs the base repo):\n%s\n\n"
            "End your reply with EXACTLY one JSON line and nothing after it: "
            "{\"verdict\":\"approve\"} if it satisfies the ask with no high-severity blocker, "
            "{\"verdict\":\"request_changes\",\"high_sev_blockers\":<N>} if there is a real "
            "blocker, or {\"verdict\":\"comment\"} if you only have non-blocking remarks.",
            persona, focus, proposal, diff);
   return buf;
}

/* The seat model bound to `persona` in the active roundtable preset, or NULL when
 * no seat matches (the caller then treats the lens as "$random"). The model may
 * itself be the "$random" sentinel — a seat the user explicitly set to random. */
static const char *seat_model_for_persona(const roundtable_preset_t *preset, const char *persona)
{
   if (!preset || !persona || !persona[0])
      return NULL;
   for (int i = 0; i < preset->seat_count; i++)
      if (strcmp(preset->seats[i].persona, persona) == 0)
         return preset->seats[i].model;
   return NULL;
}

/* Load the roundtable preset this gate convenes into *preset for its
 * persona->model bindings. `requested` is the node's params.roundtable (the gate
 * may name a specific preset); when empty it falls back to the configured default
 * (roundtable.default, else "default"). Returns 1 on success, 0 when no preset
 * loads — in which case every lens resolves as "$random", preserving the
 * pre-preset "any review-capable agent per persona" behaviour. A NAMED preset
 * that does not load is logged, since it is likely a workflow authoring error. */
static int load_panel_preset(roundtable_preset_t *preset, const char *requested)
{
   memset(preset, 0, sizeof *preset);
   config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   const char *name = "default";
   if (config_load(&cfg) == 0 && cfg.roundtable_default[0])
      name = cfg.roundtable_default;
   if (requested && requested[0])
      name = requested; /* the node's explicit choice wins over the default */
   if (roundtable_preset_load(name, preset) == 0)
      return 1;
   if (requested && requested[0])
      aimee_log(LOG_WARN, "wfe-panel",
                "roundtable preset '%s' not found -> every lens falls back to $random", requested);
   return 0;
}

/* Compose the review roundtable IN PARALLEL, honoring each required lens's seat
 * model from the active roundtable preset:
 *   - a PINNED model dispatches to that EXACT agent; if it is not enabled/routable
 *     for the review role (or its dispatch fails), the run FAILS — a pinned model
 *     is never silently swapped (returns WFE_PANEL_PINNED_FAIL);
 *   - a "$random" seat (or a lens with no matching seat) picks any review-capable
 *     agent and retries a different one on the second round if the first fails.
 * Panelists fan out concurrently via agent_run_parallel (bounded by the compute-
 * thread ceiling; a panelist past the deadline is abandoned). Reviews are
 * read-only (the `review` role grants no write tools); one hard-reset after the
 * panel defends the worktree regardless. Returns the number of lenses filled, or a
 * WFE_PANEL_* sentinel (<0). */
static int live_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)eligible;
   (void)nelig;
   if (!pkt || !pkt->workdir || !pkt->workdir[0])
      return WFE_PANEL_UNREACHABLE; /* no worktree to review in -> park */

   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   if (agent_load_config(&acfg) != 0)
      return WFE_PANEL_UNREACHABLE;

   int nlens = nreq < max ? nreq : max;
   if (nlens > WFE_PANEL_MAX)
      nlens = WFE_PANEL_MAX;

   /* Persona->model bindings for this panel come from the roundtable preset this
    * gate convenes: the node's named preset (pkt->roundtable) or the default. */
   roundtable_preset_t preset;
   int have_preset = load_panel_preset(&preset, pkt->roundtable);

   char *sysp[WFE_PANEL_MAX];            /* per-lens review-persona system prompt */
   char *usrp[WFE_PANEL_MAX];            /* per-lens user prompt (change under review) */
   int done[WFE_PANEL_MAX];              /* lens has a verdict */
   int pinned[WFE_PANEL_MAX];            /* lens pinned to a specific model */
   const char *pin_agent[WFE_PANEL_MAX]; /* resolved pinned agent name (into acfg) */
   memset(sysp, 0, sizeof sysp);
   memset(usrp, 0, sizeof usrp);
   memset(done, 0, sizeof done);
   memset(pinned, 0, sizeof pinned);
   memset(pin_agent, 0, sizeof pin_agent);

   const char *used[MAX_AGENTS];
   int nused = 0;

   /* Resolve seats up front. A pinned model that cannot be fulfilled fails the run
    * (no substitution): abort before dispatching anything. Pinned agents are added
    * to `used` so a later $random pick never collides with them (diversity). */
   int pinned_fail = 0;
   for (int i = 0; i < nlens; i++)
   {
      sysp[i] = persona_compose_delegate_prompt(required[i], pkt->workdir, "");
      usrp[i] = build_prompt(required[i], pkt);
      const char *model = have_preset ? seat_model_for_persona(&preset, required[i]) : NULL;
      if (rt_seat_is_random(model))
         continue; /* $random / unmatched -> resolved per-round below */
      int idx = -1;
      if (rt_resolve_seat_model(&acfg, model, "review", used, nused, &idx) != RT_SEAT_OK)
      {
         aimee_log(LOG_WARN, "wfe-panel", "pinned model '%s' for lens '%s' unavailable -> fail run",
                   model, required[i]);
         pinned_fail = 1;
         continue;
      }
      pinned[i] = 1;
      pin_agent[i] = acfg.agents[idx].name;
      if (nused < MAX_AGENTS)
         used[nused++] = pin_agent[i];
   }
   if (pinned_fail)
   {
      for (int i = 0; i < nlens; i++)
      {
         free(sysp[i]);
         free(usrp[i]);
      }
      return WFE_PANEL_PINNED_FAIL;
   }

   /* Snapshot HEAD once for the post-panel read-only reset. */
   char pre_head[64] = "";
   {
      const char *argv[] = {"git", "-C", pkt->workdir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (safe_exec_capture(argv, &o, 256) == 0 && o)
      {
         size_t n = strlen(o);
         while (n > 0 && (o[n - 1] == '\n' || o[n - 1] == '\r'))
            o[--n] = '\0';
         snprintf(pre_head, sizeof pre_head, "%s", o);
      }
      free(o);
   }

   run_cmd_set_cwd(pkt->workdir);

   int filled = 0;
   int pinned_dispatch_fail = 0;
   /* Deadline-bounded rounds. A pinned lens dispatches once to its fixed agent (a
    * pinned dispatch failure fails the run — no retry). A $random lens re-seats a
    * different agent on later rounds so one flaky pick doesn't degrade the gate —
    * and when NO review agent is eligible right now, the lens QUEUES: the round
    * loop sleeps and re-resolves until an agent becomes viable or the seat-wait
    * deadline expires. Only after the deadline does an unfilled/malformed lens
    * commit and the gate degrade. */
   struct timespec q0;
   clock_gettime(CLOCK_MONOTONIC, &q0);
   long seat_wait = wfe_panel_seat_wait_secs();
   int queued_logged = 0;
   for (int round = 0; filled < nlens && !pinned_dispatch_fail; round++)
   {
      struct timespec qn;
      clock_gettime(CLOCK_MONOTONIC, &qn);
      int final = (qn.tv_sec - q0.tv_sec) >= seat_wait; /* last attempt: commit as-is */
      int filled_before = filled;
      agent_task_t tasks[WFE_PANEL_MAX];
      const char *taskagent[WFE_PANEL_MAX];
      int lensmap[WFE_PANEL_MAX];
      memset(tasks, 0, sizeof tasks);
      int nt = 0;
      for (int i = 0; i < nlens; i++)
      {
         if (done[i])
            continue;
         const char *agent = NULL;
         if (pinned[i])
         {
            if (round > 0)
               continue; /* pinned does not retry */
            agent = pin_agent[i];
         }
         else
         {
            int idx = -1;
            /* Prefer a UNIQUE agent per lens (the `used` exclusion gives panel
             * diversity). But when there are fewer eligible review agents than
             * lenses, insisting on distinctness would leave the surplus lenses
             * unfilled and needlessly DEGRADE the whole gate. So on exhaustion,
             * DOWNGRADE: reuse an already-seated agent (drop the exclusion) — the
             * same model reviews this lens under its own persona prompt. The gate
             * then degrades only when NO review agent is eligible at all. */
            int reused = 0;
            rt_seat_resolve_t rc =
                rt_resolve_seat_model(&acfg, RT_SEAT_RANDOM, "review", used, nused, &idx);
            if (rc == RT_SEAT_RANDOM_EXHAUSTED)
            {
               rc = rt_resolve_seat_model(&acfg, RT_SEAT_RANDOM, "review", NULL, 0, &idx);
               reused = 1;
            }
            /* rt_resolve_seat_model only sets a valid idx on RT_SEAT_OK; bound-check
             * defensively anyway before indexing acfg.agents. */
            if (rc != RT_SEAT_OK || idx < 0 || idx >= acfg.agent_count)
               continue; /* no eligible review agent at all -> unfilled (gate degrades) */
            agent = acfg.agents[idx].name;
            if (reused)
               /* surface the degraded-diversity composition so operators can see the
                * panel ran with fewer distinct agents than lenses. */
               aimee_log(LOG_INFO, "wfe-panel",
                         "reusing agent '%s' for lens '%s' (fewer eligible review agents "
                         "than lenses)",
                         agent, required[i]);
            else if (nused < MAX_AGENTS)
               used[nused++] = agent; /* only DISTINCT picks join the diversity set */
         }
         tasks[nt].role = "review";
         tasks[nt].agent = agent;
         tasks[nt].system_prompt = sysp[i] ? sysp[i] : "";
         tasks[nt].user_prompt = usrp[i] ? usrp[i] : "";
         tasks[nt].temperature = 0.2;
         tasks[nt].max_tokens = AGENT_DEFAULT_MAX_TOKENS;
         taskagent[nt] = agent;
         lensmap[nt] = i;
         nt++;
      }
      if (nt == 0)
      {
         /* Nothing dispatchable this round: every unfilled lens is unseatable
          * (no eligible review agent right now). Queue for a seat until the
          * deadline instead of degrading instantly. */
         if (final || seat_wait == 0)
            break;
         if (!queued_logged)
         {
            aimee_log(LOG_INFO, "wfe-panel",
                      "no eligible review agent for %d unfilled lens(es); queueing up to %lds",
                      nlens - filled, seat_wait);
            queued_logged = 1;
         }
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
         continue;
      }

      agent_result_t results[WFE_PANEL_MAX];
      memset(results, 0, sizeof results);
      agent_run_parallel(&acfg, tasks, nt, results, WFE_PANEL_DEADLINE_MS);
      for (int t = 0; t < nt; t++)
      {
         int i = lensmap[t];
         if (results[t].response && results[t].response[0])
         {
            wfe_panel_verdict_from_review(required[i], pkt->artifact_hash, results[t].response,
                                          &out[filled]);
            snprintf(out[filled].model, sizeof out[filled].model, "%s", taskagent[t]);
            /* Attribute the verdict so a degraded gate is triageable: without
             * this there is no record of WHICH agent served a lens or what it
             * returned. On MALFORMED include the reply's tail — that is the
             * line the parser rejected. */
            {
               static const char *const kind_names[] = {"approve", "request_changes", "comment",
                                                        "malformed"};
               wfe_verdict_kind_t k = out[filled].kind;
               const char *kn = (k >= 0 && k <= WFE_V_MALFORMED) ? kind_names[k] : "?";
               if (k == WFE_V_MALFORMED)
               {
                  const char *r = results[t].response;
                  size_t rl = strlen(r);
                  const char *tail = rl > 160 ? r + rl - 160 : r;
                  aimee_log(LOG_WARN, "wfe-panel",
                            "lens '%s' verdict malformed from agent '%s'; reply tail: %.160s",
                            required[i], taskagent[t], tail);
                  /* A malformed reply from a $random seat is provider flakiness
                   * (typically a review truncated before its final JSON line, as
                   * the tail above shows) — treat it like a failed dispatch and
                   * let the next round retry the lens with a DIFFERENT agent
                   * rather than committing a verdict that fails required-lens
                   * coverage and degrades the whole gate. Only once the seat-wait
                   * deadline expires does the malformed verdict commit
                   * (fail-closed as before). */
                  if (!pinned[i] && !final)
                  {
                     free(results[t].response);
                     results[t].response = NULL;
                     continue; /* leave done[i]=0 -> a later round re-seats this lens */
                  }
               }
               else
                  aimee_log(LOG_INFO, "wfe-panel", "lens '%s' verdict %s from agent '%s'",
                            required[i], kn, taskagent[t]);
            }
            filled++;
            done[i] = 1;
         }
         else if (pinned[i])
         {
            /* Reachable at resolve time but its dispatch failed -> still "cannot be
             * fulfilled" for a pinned model, so fail the run (no substitution). */
            aimee_log(LOG_WARN, "wfe-panel",
                      "pinned model '%s' for lens '%s' failed to respond -> fail run", taskagent[t],
                      required[i]);
            pinned_dispatch_fail = 1;
         }
         free(results[t].response);
      }
      /* No lens progressed this round (fast dispatch failures / all replies
       * malformed): back off before re-seating so a flapping provider doesn't
       * burn the whole seat-wait window in a tight retry loop. */
      if (filled == filled_before && filled < nlens && !pinned_dispatch_fail && !final &&
          seat_wait > 0)
      {
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
      }
   }

   run_cmd_set_cwd(NULL);

   /* Read-only enforcement: hard-reset to the pre-panel HEAD + clean untracked. */
   if (pre_head[0])
   {
      const char *reset[] = {"reset", "--hard", pre_head};
      panel_git(pkt->workdir, reset, 3);
      const char *clean[] = {"clean", "-fd"};
      panel_git(pkt->workdir, clean, 2);
   }

   for (int i = 0; i < nlens; i++)
   {
      if (!done[i] && !pinned[i] && !pinned_dispatch_fail)
         aimee_log(LOG_WARN, "wfe-panel", "no viable review agent for lens '%s' -> degrade",
                   required[i]);
      free(sysp[i]);
      free(usrp[i]);
   }
   if (pinned_dispatch_fail)
      return WFE_PANEL_PINNED_FAIL;
   return filled;
}

void wfe_live_panel_register(void)
{
   wfe_set_panel_provider(live_panel);
   aimee_log(LOG_INFO, "wfe-panel", "live roundtable panel provider registered");
}

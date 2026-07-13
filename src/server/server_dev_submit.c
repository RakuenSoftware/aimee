/* server_dev_submit.c: the shared autonomous-run intake (dev_submit_run) behind
 * POST /v1/dev/submit and the `workflow_run` MCP tool. Split out of
 * server_http_routes.c (which was at the line-check ceiling); the route handler
 * rh_dev_submit and the MCP handler both call dev_submit_run, declared in
 * server_http.h. */
#include "server_http.h" /* dev_submit_run contract */
#include "cJSON.h"
#include "aimee_home.h"    /* aimee_home */
#include "router_advise.h" /* router_autonomous_pick / router_autonomous_audit */
#include "wfe_autonomy.h"  /* wfe_autonomy_default_max_cost_usd — shared intake cap policy */
#include "wfe_engine.h"    /* wfe_work_item_resolve */
#include "wfe_scheduler.h" /* wfe_scheduler_notify */
#include "wfe_store.h"     /* db1_work_item_submit_capped / _set_terminal / _set_cost_cap */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Parse a positive-integer env cap value. Returns `defval` for unset/empty/
 * non-numeric/trailing-garbage/overflow/out-of-[1,1000000] — so a malformed
 * AIMEE_AUTONOMY_* (incl. "0" or a negative) can never silently DISABLE the cap,
 * only fall back to the safe default. The whole string must be consumed. The
 * caller fetches the value via a getenv string literal so the var stays
 * discoverable by the docs-gen scanner. */
static int dev_env_cap(const char *v, int defval)
{
   if (!v || !v[0])
      return defval;
   errno = 0;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (errno != 0 || !end || *end != '\0' || n < 1 || n > 1000000)
      return defval;
   return (int)n;
}

/* AIMEE_WORKFLOW_AUTONOMOUS_ROUTER (S4): default-OFF. On -> an omitted `workflow`
 * on /v1/dev/submit is routed (clamped to the full-spine set) instead of
 * defaulting to "build". Same truthy set as the other aimee boolean flags. */
static int dev_autonomous_router_on(void)
{
   const char *v = getenv("AIMEE_WORKFLOW_AUTONOMOUS_ROUTER");
   return v && (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0);
}

/* Shared autonomous-run intake (declared in server_http.h) — the capped/audited
 * core behind POST /v1/dev/submit and the `workflow_run` MCP tool. See the header
 * for the contract. `submitter` is supplied by the caller (the HTTP route passes
 * the attested principal; the MCP tool passes the connection principal) so this
 * body is transport-agnostic. */
int dev_submit_run(const char *proposal_md, const char *workflow_opt, const char *repo,
                   const char *submitter, cJSON **out, char *err, size_t errlen)
{
   if (out)
      *out = NULL;
   if (!proposal_md || !proposal_md[0])
   {
      snprintf(err, errlen, "proposal_md is required");
      return 400;
   }
   if (!repo)
      repo = "";

   /* Workflow selection. Explicit `workflow_opt` ALWAYS wins (backwards-compat).
    * When NULL, S4 autonomous parity (default-OFF via AIMEE_WORKFLOW_AUTONOMOUS_ROUTER)
    * routes the proposal through the router (clamped to the full-spine set, floor
    * managed-change) instead of silently defaulting to "build". */
   char wf_choice[64];
   int autoroute = 0, autoroute_clamped = 0;
   char autoroute_src[16] = "", autoroute_raw[64] = "", autoroute_tag[9] = "";
   if (workflow_opt && workflow_opt[0])
   {
      snprintf(wf_choice, sizeof wf_choice, "%s", workflow_opt); /* explicit: authoritative */
   }
   else if (dev_autonomous_router_on())
   {
      autoroute = 1; /* pick fills wf_choice + audit fields */
      router_autonomous_pick(proposal_md, wf_choice, sizeof wf_choice, autoroute_src,
                             sizeof autoroute_src, autoroute_raw, sizeof autoroute_raw,
                             autoroute_tag, sizeof autoroute_tag, &autoroute_clamped);
   }
   else
   {
      snprintf(wf_choice, sizeof wf_choice, "build"); /* legacy default */
   }
   const char *workflow = wf_choice;

   /* Intake-auth (WP-4/WP-5): require an attested submitter, and cap per-principal
    * concurrency + submit rate so one principal can't fan out unbounded autonomous
    * runs. Binds the run to the principal for audit and bounds resource abuse.
    * Both caps are DB-backed (survive restart). */
   if (!submitter || !submitter[0])
   {
      snprintf(err, errlen, "unauthenticated: no attested principal");
      return 401;
   }
   /* The submitter is the cap/audit key stored in submitter[128]; a longer
    * principal would truncate and collide with another's quota bucket. */
   if (strlen(submitter) >= 128)
   {
      snprintf(err, errlen, "principal too long");
      return 400;
   }
   int max_active = dev_env_cap(getenv("AIMEE_AUTONOMY_MAX_ACTIVE_PER_PRINCIPAL"), 5);
   int rate_max = dev_env_cap(getenv("AIMEE_AUTONOMY_SUBMIT_RATE_PER_MIN"), 10);
   int rate_secs = dev_env_cap(getenv("AIMEE_AUTONOMY_SUBMIT_WINDOW_SECS"), 60);

   /* Compute the unique artifact path now, but DON'T write the file until the
    * submit is admitted below — a capped/rate-limited/faulted principal must not be
    * able to land an unbounded proposal file on disk on every rejected request. */
   const char *home = aimee_home();
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows/proposals", home ? home : "/tmp");
   char ppath[1152];
   snprintf(ppath, sizeof ppath, "%s/wi-%ld-%d.md", dir, (long)time(NULL), (int)getpid());

   /* Resolve the workflow (no DB write), then create + cap-check + submitter-bind +
    * audit ATOMICALLY under one BEGIN IMMEDIATE so concurrent submits from one
    * principal serialize (TOCTOU-free) and any DB fault fails closed. */
   char id[80] = "", rerr[256] = "";
   char wf_name[64], wf_ver[65], wf_start[64], wf_repo[512];
   if (wfe_work_item_resolve(workflow, repo, wf_name, wf_ver, wf_start, wf_repo, id, rerr,
                             sizeof rerr) != 0 ||
       !id[0])
   {
      snprintf(err, errlen, "%s", rerr[0] ? rerr : "failed to resolve workflow");
      return 500;
   }
   int sr = db1_work_item_submit_capped(id, wf_repo, ppath, wf_name, wf_ver, wf_start, submitter,
                                        max_active, rate_max, rate_secs);
   if (sr == 1)
   {
      snprintf(err, errlen, "too many active autonomous runs for this principal");
      return 429;
   }
   if (sr == 2)
   {
      snprintf(err, errlen, "submit rate exceeded for this principal");
      return 429;
   }
   if (sr != 0)
   {
      /* Fail CLOSED: a DB/txn fault on the cap path returns 503, never an
       * uncapped/unattributed run. */
      snprintf(err, errlen, "could not record submission");
      return 503;
   }
   /* Admitted: now (and only now) write the proposal artifact the row references.
    * If the write fails, abandon the just-created run so it can't drive a missing
    * proposal forward. */
   mkdir(dir, 0700); /* best effort; parent created by standup */
   FILE *pf = fopen(ppath, "wb");
   if (pf)
   {
      fwrite(proposal_md, 1, strlen(proposal_md), pf);
      fclose(pf);
   }
   if (!pf)
   {
      db1_work_item_set_terminal(id, "abandoned");
      snprintf(err, errlen, "could not persist proposal");
      return 500;
   }
   if (autoroute) /* S4: audit the routing decision (typed fields; no proposal content) */
      router_autonomous_audit(id, workflow, autoroute_src, autoroute_raw, autoroute_clamped,
                              autoroute_tag);
   /* WP-5: a per-run USD budget ceiling so a runaway autonomous run parks
    * (budget_exceeded) instead of burning unbounded delegate cost. The default
    * policy ($5.00, AIMEE_AUTONOMY_MAX_USD override, 0 disables) is shared with
    * every other autonomous intake (wfe_autonomy_default_max_cost_usd). */
   {
      double cap_usd = wfe_autonomy_default_max_cost_usd();
      if (cap_usd > 0)
         db1_work_item_set_cost_cap(id, cap_usd);
   }
   wfe_scheduler_notify(); /* drive it now; the run continues server-side */
   if (out)
   {
      cJSON *ok = cJSON_CreateObject();
      cJSON_AddStringToObject(ok, "work_item_id", id);
      cJSON_AddStringToObject(ok, "workflow", workflow);
      cJSON_AddStringToObject(ok, "state", "active");
      *out = ok;
   }
   return 200;
}

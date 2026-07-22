/* server_skill_jobs.c: fire-and-forget skill review and curator job dispatch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_compute_impl.h"
#include "skill_curator.h"
#include "gw_orch_delegates.h"
#include "config.h"
#include "log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>

/* Fields the spawn adapter needs beyond (role, brief), carried as the orchestration
 * capability's opaque ctx. spawn_rc records the on-demand submit outcome so the caller can
 * warn on a genuine submit failure (0 spawned, -1 refused/failed; -1 until the adapter runs). */
typedef struct
{
   server_ctx_t *server;
   int spawn_rc;
} skill_review_backing_t;

/* The spawn_delegate capability for the skill-review job: build the delegate request + compute
 * ctx from (role, brief) and submit it to an on-demand delegate worker. Mirrors the coord
 * dispatcher's adapter (server_coord_dispatcher.c). Records the outcome in the backing. */
static int skill_review_spawn_delegate(void *ctx, const char *role, const char *brief)
{
   skill_review_backing_t *b = (skill_review_backing_t *)ctx;
   if (!b)
      return -1;
   b->spawn_rc = -1;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "role", role && role[0] ? role : "review");
   cJSON_AddStringToObject(req, "prompt", brief ? brief : "");
   cJSON_AddTrueToObject(req, "handoff_json");
   compute_ctx_t *cctx = calloc(1, sizeof(compute_ctx_t));
   if (!cctx)
   {
      cJSON_Delete(req);
      return -1;
   }
   cctx->server = b->server;
   cctx->conn_fd = -1;
   cctx->async_slot = -1;
   cctx->req = req;
   /* Skill-review delegates are I/O-bound; run on-demand, not the CPU compute
    * pool (see delegate_spawn_ondemand). */
   if (delegate_spawn_ondemand(cctx) != 0)
   {
      compute_ctx_free(cctx);
      b->spawn_rc = -1;
      return -1;
   }
   b->spawn_rc = 0;
   return 0;
}

/* Resolve whether the delegates module is enabled: the config-store `modules.delegates` toggle
 * (canonical), falling back to the deprecated env default. Loaded per call (mtime-cached), so an
 * operator toggle takes effect without a restart. Keeps the pure gw_orch_delegates module
 * config-free (mirrors coord_delegates_enabled in server_coord_dispatcher.c). */
static int skill_review_delegates_enabled(void)
{
   config_t cfg;
   int tri = (config_load(&cfg) == 0) ? cfg.module_delegates : -1;
   return config_module_enabled(tri, gw_orch_delegates_enabled());
}

void server_compute_skill_review_async(server_ctx_t *ctx, const char *session_id)
{
   if (!ctx || !session_id || !session_id[0])
      return;

   char prompt[512];
   snprintf(prompt, sizeof(prompt),
            "Review the just-completed session (id=%s) for skill improvement opportunities. "
            "For each skill activated, consider: (a) did the session reveal a class-level "
            "pattern the skill should cover? (b) is there guidance worth lifting into a new "
            "skill? Use skill_manage to propose changes. Names must be class-level (no PR "
            "numbers, no session artifacts). Prefer patching existing skills over creating new "
            "ones. A no-op pass is acceptable.",
            session_id);

   /* Route the spawn through the DELEGATES orchestration module so it is a togglable, registered
    * hook rather than an inline imperative call (see gw_orch_delegates), matching the coord
    * dispatcher port. Fire-and-forget: on a disabled module we skip the review, and only warn on
    * a genuine submit failure — the two are logged distinctly. */
   skill_review_backing_t backing = {ctx, -1};
   gw_turn_capabilities_t caps = {&backing, skill_review_spawn_delegate, NULL};
   char turn_id[64];
   snprintf(turn_id, sizeof(turn_id), "skill-review-%s", session_id);
   if (gw_orch_delegates_run(&caps, turn_id, "review", prompt, skill_review_delegates_enabled()) !=
       0)
   {
      /* Module disabled (or catalog build failed): no spawn attempted. A disabled module is an
       * intentional operator choice, not a failure. */
      LOG_INFO("skill_review", "delegates module disabled — skipping review for session %s",
               session_id);
      return;
   }
   if (backing.spawn_rc != 0)
      LOG_WARN("skill_review", "failed to submit review job for session %s", session_id);
}

void server_compute_skill_curator_async(server_ctx_t *ctx)
{
   (void)ctx;
   skill_curator_maybe();
}

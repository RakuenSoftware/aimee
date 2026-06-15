/* server_skill_jobs.c: fire-and-forget skill review and curator job dispatch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_compute_impl.h"
#include "skill_curator.h"
#include "log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>

void server_compute_skill_review_async(server_ctx_t *ctx, const char *session_id)
{
   if (!ctx || !session_id || !session_id[0])
      return;

   cJSON *req = cJSON_CreateObject();
   if (!req)
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

   cJSON_AddStringToObject(req, "role", "review");
   cJSON_AddStringToObject(req, "prompt", prompt);
   cJSON_AddTrueToObject(req, "handoff_json");

   compute_ctx_t *cctx = calloc(1, sizeof(compute_ctx_t));
   if (!cctx)
   {
      cJSON_Delete(req);
      return;
   }
   cctx->server = ctx;
   cctx->conn_fd = -1;
   cctx->async_slot = -1;
   cctx->req = req;

   /* Skill-review delegates are I/O-bound; run on-demand, not the CPU compute
    * pool (see delegate_spawn_ondemand). */
   if (delegate_spawn_ondemand(cctx) != 0)
   {
      compute_ctx_free(cctx);
      LOG_WARN("skill_review", "failed to submit review job for session %s", session_id);
   }
}

void server_compute_skill_curator_async(server_ctx_t *ctx)
{
   (void)ctx;
   skill_curator_maybe();
}

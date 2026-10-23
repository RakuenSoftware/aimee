/* aimee_ir_shadow.c -- see aimee_ir_shadow.h. */
#include "aimee.h" /* MAX_PATH_LEN et al: agent_types.h depends on it */

#include "aimee_ir_shadow.h"

#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/aimee_ir_metrics.h>
#include "cJSON.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap the mismatch/failure log so a systematic bug can't flood server.log. */
static atomic_int g_logged;
#define SHADOW_LOG_CAP 20

static int shadow_enabled(void)
{
   const char *v = getenv("AIMEE_IR_SHADOW");
   return v && v[0] && v[0] != '0';
}

void aimee_ir_shadow_observe_request(const cJSON *req, aimee_wire_t frontend)
{
   if (!shadow_enabled() || !req)
      return;
   /* Slice 3 starts with the Anthropic frontend (Claude Code, the primary case). */
   if (frontend != AIMEE_WIRE_ANTHROPIC)
      return;

   aimee_request_t ir;
   char err[128];
   if (anthropic_frontend_parse(req, &ir, err, sizeof err) != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, frontend);
      if (g_logged < SHADOW_LOG_CAP)
      {
         fprintf(stderr, "[ir-shadow] anthropic parse failed: %s\n", err);
         g_logged++;
      }
      return;
   }
   aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, frontend);

   /* The byte-parity gate was retired with the raw sidecar (the canonical egress now
    * INTENTIONALLY re-renders every request, so byte-parity vs the client's raw bytes
    * is meaningless). But the residual risk it implicitly covered -- a top-level field
    * the IR does not model being silently DROPPED from the canonical egress -- remains.
    * So the shadow is re-purposed to FIELD-COVERAGE detection: every top-level key the
    * client sent must be one the IR models. A request using only modeled keys scores
    * REBUILD_MATCH; one carrying an unmodeled top-level key scores REBUILD_MISMATCH and
    * logs the key NAME (never its value -- no request content in logs), flagging a
    * modeling gap to close. Cheap: a key-name scan, no rebuild. */
   static const char *const MODELED[] = {
       "model",        "max_tokens",  "messages", "system",         "tools",
       "tool_choice",  "temperature", "top_p",    "top_k",          "metadata",
       "service_tier", "thinking",    "stream",   "stop_sequences", NULL};
   const char *unmodeled = NULL;
   for (const cJSON *k = req->child; k; k = k->next)
   {
      if (!k->string)
         continue;
      int modeled = 0;
      for (const char *const *m = MODELED; *m; m++)
         if (strcmp(k->string, *m) == 0)
         {
            modeled = 1;
            break;
         }
      if (!modeled)
      {
         unmodeled = k->string;
         break;
      }
   }
   if (!unmodeled)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_REBUILD_MATCH, frontend);
   }
   else
   {
      aimee_ir_metric_inc(AIMEE_IR_M_REBUILD_MISMATCH, frontend);
      if (g_logged < SHADOW_LOG_CAP)
      {
         g_logged++;
         fprintf(stderr, "[ir-shadow] anthropic canonical egress drops unmodeled top-level '%s'\n",
                 unmodeled);
      }
   }
   aimee_request_free(&ir);
}

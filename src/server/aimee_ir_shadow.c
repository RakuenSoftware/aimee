/* aimee_ir_shadow.c -- see aimee_ir_shadow.h. */
#include "aimee_ir_shadow.h"

#include "aimee_backend.h"
#include "aimee_frontend.h"
#include "aimee_ir_metrics.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap the mismatch/failure log so a systematic bug can't flood server.log. */
static int g_logged;
#define SHADOW_LOG_CAP 20

static int shadow_enabled(void)
{
   const char *v = getenv("AIMEE_IR_SHADOW");
   return v && v[0] && v[0] != '0';
}

int aimee_ir_shadow_enabled(void)
{
   return shadow_enabled();
}

void aimee_ir_shadow_compare_bodies(const char *ir_body, const char *legacy_body,
                                    aimee_wire_t frontend)
{
   if (!shadow_enabled())
      return;
   /* A NULL on either side is a divergence, not a skip: "the IR could not build it"
    * is exactly the case that forces a legacy fallback in production. */
   if (ir_body && legacy_body && strcmp(ir_body, legacy_body) == 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BODY_MATCH, frontend);
      return;
   }
   aimee_ir_metric_inc(AIMEE_IR_M_BODY_MISMATCH, frontend);
   if (g_logged < SHADOW_LOG_CAP)
   {
      g_logged++;
      /* Truncated on purpose: bodies carry user content, and the roundtable ruling
       * on this refactor was explicit that no raw request bytes may reach the log.
       * The lengths + a short prefix are enough to identify WHICH field diverged;
       * reproduce the full diff offline from the shape, not from production logs. */
      fprintf(stderr,
              "aimee_ir_shadow: provider-body mismatch (wire=%d) ir_len=%zu legacy_len=%zu\n"
              "  ir[0:80]     =%.80s\n"
              "  legacy[0:80] =%.80s\n",
              (int)frontend, ir_body ? strlen(ir_body) : 0, legacy_body ? strlen(legacy_body) : 0,
              ir_body ? ir_body : "(null)", legacy_body ? legacy_body : "(null)");
   }
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

   /* rebuild same-protocol and check the round-trip is IR-stable */
   cJSON *rebuilt = anthropic_backend_build(&ir);
   if (!rebuilt)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, frontend);
   }
   else
   {
      aimee_request_t ir2;
      if (anthropic_frontend_parse(rebuilt, &ir2, err, sizeof err) == 0)
      {
         if (!aimee_ir_request_equal(&ir, &ir2))
         {
            aimee_ir_metric_inc(AIMEE_IR_M_REBUILD_MISMATCH, frontend);
            if (g_logged < SHADOW_LOG_CAP)
            {
               fprintf(stderr, "[ir-shadow] anthropic round-trip MISMATCH (n_msgs=%d n_tools=%d)\n",
                       ir.n_messages, ir.n_tools);
               g_logged++;
            }
         }
         aimee_request_free(&ir2);
      }
      else
      {
         aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, frontend);
      }
      cJSON_Delete(rebuilt);
   }
   aimee_request_free(&ir);
}

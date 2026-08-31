/* wfe_verdict.c: the fail-closed roundtable gate decision.
 *
 * The decision RULE moved to the workflows module
 * (server-go/modules/workflows/gate.go) and is reached as bus stage
 * AIMEE_WORKFLOWS_STAGE_GATE_DECIDE. It is pure policy over a panel's verdicts,
 * which is feature work rather than transport; what stays here is shaping the
 * request and mapping the ruling back.
 *
 * One call per gate, not per verdict: the whole panel crosses in a single
 * message, so migrating it costs one round trip rather than N.
 *
 * Fail-closed survives the move intact, and gets one more guard. An unreachable
 * module, a malformed reply or an unrecognised decision all yield DEGRADED --
 * "the panel could not be composed" -- which parks the workflow for a human. It
 * must never be able to yield APPROVE, which would advance a workflow on a gate
 * that never actually ran. */
#include "wfe_verdict.h"

#include "cJSON.h"
#include "headers/module_json_call.h"

#include <aimee/workflows/module_api.h>
#include <stdio.h>
#include <string.h>

/* A panel is a handful of verdicts carrying only short identity fields (the
 * critique text is deliberately not sent -- the ruling does not read it). */
#define WFE_GATE_MAX_BODY (256u * 1024u)
/* The gate is off the interactive path but a workflow waits on it, so the
 * deadline is generous rather than tight; exceeding it degrades, never approves. */
#define WFE_GATE_TIMEOUT_MS 15000

int wfe_gate_effective_quorum(int requested, int nreq)
{
   /* Kept in C: three comparisons that every caller needs before it can even
    * build the request, so a round trip would be pure cost. Mirrored in Go as
    * EffectiveQuorum for module-side callers. */
   int q = requested;
   if (q < nreq)
      q = nreq;
   if (q < 2)
      q = 2; /* single-lens floor */
   return q;
}

wfe_gate_decision_t wfe_gate_decide(const wfe_verdict_t *verdicts, int n,
                                    const char *const *required, int nreq, int quorum,
                                    const char *artifact_hash, char *reason, size_t rcap)
{
   if (reason && rcap)
      reason[0] = '\0';

   cJSON *request = cJSON_CreateObject();
   if (!request)
      return WFE_GATE_DEGRADED;

   cJSON *panel = cJSON_AddArrayToObject(request, "verdicts");
   cJSON *req = cJSON_AddArrayToObject(request, "required");
   if (!panel || !req)
   {
      cJSON_Delete(request);
      return WFE_GATE_DEGRADED;
   }
   for (int i = 0; i < n; i++)
   {
      cJSON *v = cJSON_CreateObject();
      if (!v)
      {
         cJSON_Delete(request);
         return WFE_GATE_DEGRADED;
      }
      /* model and feedback are omitted deliberately: the ruling does not read
       * them, and feedback is up to a kilobyte per panelist. */
      cJSON_AddStringToObject(v, "persona", verdicts[i].persona);
      cJSON_AddNumberToObject(v, "schema_version", verdicts[i].schema_version);
      cJSON_AddStringToObject(v, "reviewed_content_hash", verdicts[i].reviewed_content_hash);
      cJSON_AddNumberToObject(v, "kind", (double)verdicts[i].kind);
      cJSON_AddNumberToObject(v, "high_sev_blockers", verdicts[i].high_sev_blockers);
      cJSON_AddItemToArray(panel, v);
   }
   for (int r = 0; r < nreq; r++)
      cJSON_AddItemToArray(req, cJSON_CreateString(required[r] ? required[r] : ""));
   cJSON_AddNumberToObject(request, "quorum", quorum);
   cJSON_AddStringToObject(request, "artifact_hash", artifact_hash ? artifact_hash : "");

   cJSON *reply =
       aimee_module_json_call(AIMEE_WORKFLOWS_EVENT_GATE_DECIDE, AIMEE_WORKFLOWS_STAGE_GATE_DECIDE,
                              request, WFE_GATE_MAX_BODY, WFE_GATE_TIMEOUT_MS, NULL);
   if (!reply)
   {
      if (reason)
         snprintf(reason, rcap, "gate module unavailable or returned an unreadable ruling");
      return WFE_GATE_DEGRADED;
   }

   const cJSON *decision = cJSON_GetObjectItemCaseSensitive(reply, "decision");
   const cJSON *why = cJSON_GetObjectItemCaseSensitive(reply, "reason");
   const char *name = cJSON_IsString(decision) ? decision->valuestring : "";

   wfe_gate_decision_t ruled;
   if (strcmp(name, "approve") == 0)
      ruled = WFE_GATE_APPROVE;
   else if (strcmp(name, "changes") == 0)
      ruled = WFE_GATE_CHANGES;
   else if (strcmp(name, "degraded") == 0)
      ruled = WFE_GATE_DEGRADED;
   else
   {
      /* An unrecognised ruling is not a gate result. Degrade rather than guess. */
      if (reason)
         snprintf(reason, rcap, "gate module returned an unrecognised decision");
      cJSON_Delete(reply);
      return WFE_GATE_DEGRADED;
   }

   if (reason && rcap && cJSON_IsString(why))
      snprintf(reason, rcap, "%s", why->valuestring);
   cJSON_Delete(reply);
   return ruled;
}

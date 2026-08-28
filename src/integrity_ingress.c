/* One enforcement/evidence seam for every content materialization boundary. */
#include "integrity.h"
#include "config.h"
#include <aimee/audit/obs_bus.h>

#include <stdio.h>
#include <string.h>

static const char *safe_boundary(const char *boundary)
{
   static const char *allowed[] = {"document",  "pdf",        "memory",   "recall",
                                   "retrieval", "attachment", "learning", "stored_content",
                                   NULL};
   for (int i = 0; allowed[i]; i++)
      if (boundary && !strcmp(boundary, allowed[i]))
         return allowed[i];
   return "unknown";
}

int integrity_ingress_decide(const char *text, integrity_source_t source, const char *boundary,
                             int autonomous, integrity_result_t *result_out)
{
   integrity_result_t result = integrity_gate_check(text, source);
   if (result_out)
      *result_out = result;
   int enabled = !config_present() || config_integrity_enabled();
   int dry_run = config_present() && config_integrity_dry_run();
   int enforce = result.verdict != INTEGRITY_VERDICT_ACCEPT &&
                 ((autonomous && source != INTEGRITY_SOURCE_USER_STATED) || (enabled && !dry_run));
   char detail[256];
   snprintf(detail, sizeof(detail),
            "{\"source\":\"%s\",\"category\":\"%s\",\"autonomous\":%s,"
            "\"enabled\":%s,\"dry_run\":%s,\"enforced\":%s}",
            integrity_source_name(source), result.match_category, autonomous ? "true" : "false",
            enabled ? "true" : "false", dry_run ? "true" : "false", enforce ? "true" : "false");
   obs_bus_emit_durable_event("integrity.ingress", safe_boundary(boundary),
                              integrity_verdict_name(result.verdict), detail);
   return enforce ? 1 : 0;
}

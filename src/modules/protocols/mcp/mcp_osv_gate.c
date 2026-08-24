/* mcp_osv_gate.c: see mcp_osv_gate.h.
 *
 * Moved verbatim out of mcp_client_registry.c (registry_osv_blocks_client /
 * registry_target_allowlisted) so the plugin-module admission path executes the
 * same bytes rather than a lookalike. The only change is that it takes argv
 * directly instead of a config_mcp_client_t, because a plugin module's command
 * does not come from aimee.yaml. */
#include "aimee/protocols/mcp/mcp_osv_gate.h"

#include "config.h"
#include "log.h"
#include "mcp_osv_cache.h"
#include "osv_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void gate_warn(const char *name, const char *fmt, ...)
{
   char detail[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(detail, sizeof(detail), fmt, ap);
   va_end(ap);
   if (name && name[0])
      LOG_WARN("mcp-osv", "%s: %s", name, detail);
   else
      LOG_WARN("mcp-osv", "%s", detail);
}

static int gate_target_allowlisted(const osv_target_t *target)
{
   if (!target || !target->ecosystem[0] || !target->name[0])
      return 0;

   char key[256];
   snprintf(key, sizeof(key), "%s:%s", target->ecosystem, target->name);
   for (int i = 0; i < config_mcp_osv_allow_count(); i++)
   {
      if (strcmp(config_mcp_osv_allow(i), key) == 0)
         return 1;
   }
   return 0;
}

int mcp_osv_gate_blocks_argv(const char *name, int argc, const char *const argv[])
{
   if (!config_mcp_osv_enabled() || argc <= 0 || !argv)
      return 0;

   osv_target_t target;
   if (osv_infer_target_from_argv(argc, argv, &target) != 0)
      return 0;

   osv_result_t result =
       osv_check_cached(config_mcp_osv_endpoint(), &target, config_mcp_osv_cache_ttl_hours(),
                        config_mcp_osv_offline(), 10000);
   int allowlisted = gate_target_allowlisted(&target);
   if (allowlisted)
   {
      if (result.verdict == OSV_VERDICT_MALWARE)
         gate_warn(name, "OSV allowlist permits %s:%s despite advisories: %s", target.ecosystem,
                   target.name, result.advisory_ids[0] ? result.advisory_ids : "MAL-*");
      (void)db1_mcp_osv_audit(name, target.ecosystem, target.name, target.version,
                              result.verdict == OSV_VERDICT_MALWARE ? "malware"
                              : result.verdict == OSV_VERDICT_CLEAN ? "clean"
                                                                    : "unknown",
                              result.verdict == OSV_VERDICT_MALWARE ? "allow_allowlisted" : "allow",
                              result.advisory_ids);
      return 0;
   }

   if (result.verdict == OSV_VERDICT_MALWARE)
   {
      gate_warn(name, "%s %s:%s has malware advisories: %s",
                config_mcp_osv_enforce() ? "blocked" : "shadow-block", target.ecosystem,
                target.name, result.advisory_ids[0] ? result.advisory_ids : "MAL-*");
      (void)db1_mcp_osv_audit(name, target.ecosystem, target.name, target.version, "malware",
                              config_mcp_osv_enforce() ? "block" : "shadow_block",
                              result.advisory_ids);
      return config_mcp_osv_enforce() ? 1 : 0;
   }

   (void)db1_mcp_osv_audit(name, target.ecosystem, target.name, target.version,
                           result.verdict == OSV_VERDICT_CLEAN ? "clean" : "unknown", "allow",
                           result.advisory_ids);
   if (result.verdict == OSV_VERDICT_UNKNOWN)
   {
      if (config_mcp_osv_offline())
         gate_warn(name, "OSV offline/cache miss: allowing %s:%s", target.ecosystem, target.name);
      else
         gate_warn(name, "OSV check unavailable: allowing %s:%s", target.ecosystem, target.name);
   }
   return 0;
}

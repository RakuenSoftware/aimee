/* kb_mcp_osv_stub.c: aimee-kb-only stubs for the DB1-backed MCP OSV cache/audit.
 *
 * When aimee-kb HOSTS MCP plugins (install: kb), it boots the same
 * mcp_client_registry the server uses, which runs the OSV supply-chain scan
 * (registry_osv_blocks_client -> osv_check_cached) before starting each plugin.
 * That live scan is a SECURITY GATE and MUST run on the kb too — otherwise
 * `install: kb` would silently bypass the malware check that `install: server`
 * enforces, an asymmetry an attacker could abuse.
 *
 * osv_check_cached() and the registry reach for three DB1 symbols
 * (db1_mcp_osv_cache_get/upsert + db1_mcp_osv_audit). DB1 (sqlite) is a
 * server-only store; aimee-kb never links it. These stubs satisfy the linker
 * WITHOUT pulling DB1 into aimee-kb, mirroring kb_obs_bus_stub.c:
 *
 *   - cache_get   -> always MISS, so every kb boot performs a fresh live OSV
 *                    check (safe: boot-time, infrequent). The verdict — and thus
 *                    the block/shadow-block decision — is fully preserved.
 *   - cache_upsert-> no-op (no persistent cache on the kb side).
 *   - audit       -> no-op. The DECISION still fires; only the persistent OSV
 *                    audit ROW is dropped on the kb. Phase-1 gap: kb-hosted
 *                    plugin OSV verdicts are not yet persisted. Tracked for the
 *                    Phase-1 kb audit increment (obs_bus outcome reuse). */

#include "mcp_osv_cache.h"
#include "interaction_events.h"

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version,
                          int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)ttl_hours;
   (void)out;
   return -1; /* miss -> caller performs a live OSV check */
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version,
                             const char *verdict, const char *advisory_ids)
{
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)verdict;
   (void)advisory_ids;
   return 0; /* no persistent cache on the kb */
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name,
                      const char *version, const char *verdict, const char *action,
                      const char *advisory_ids)
{
   (void)client_name;
   (void)ecosystem;
   (void)name;
   (void)version;
   (void)verdict;
   (void)action;
   (void)advisory_ids;
   return 0; /* decision preserved; persistent audit row deferred (Phase-1 gap) */
}

/* server/failover.c (pulled in by osv_check's HTTP client) records failover
 * events as DB1 interaction events. aimee-kb has no DB1 interaction-events table;
 * drop them here rather than link DB1. Failover still functions — only the
 * telemetry row is elided. */
int db1_interaction_event_record(const char *session_id, const char *type_name, const char *actor,
                                 const char *payload_json, const char *outcome)
{
   (void)session_id;
   (void)type_name;
   (void)actor;
   (void)payload_json;
   (void)outcome;
   return 0;
}

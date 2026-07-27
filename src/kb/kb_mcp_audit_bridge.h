#ifndef DEC_KB_MCP_AUDIT_BRIDGE_H
#define DEC_KB_MCP_AUDIT_BRIDGE_H 1

/* Record one kb-hosted MCP plugin tool-call OUTCOME on aimee-kb's OWN audit bus.
 *
 * Declared here WITHOUT a bus header so the kb service handler (kb_service_agent.c)
 * can fire it while staying bus-free; the .c file is the sole bus edge, mirroring
 * kb_memory_audit_bridge.c (D7 — the bus stays confined to the trusted daemons and
 * to a small set of bridge files). Content-free by contract: only the caller
 * identity, the tool name, a name-only args fingerprint, and classified enums
 * cross — never argument or result content, and never the raw error text the
 * plugin returned.
 *
 * Fields map onto obs_bus_emit's fixed contract:
 *   actor       the principal that invoked the tool (the calling server's dispatch
 *               role, threaded over the mcp.call request), or "mcp" when absent
 *   tool        the namespaced "<client>:<tool>" name (non-content, length-clamped)
 *   mode        "outbound" — the kb executed a plugin it hosts
 *   reason_code "" on success, "tool_error" when the plugin call failed
 *   verdict     "ok" | "error" */
void kb_mcp_audit_record(const char *actor, const char *tool, const char *mode,
                         const char *reason_code, const char *verdict);

#endif /* DEC_KB_MCP_AUDIT_BRIDGE_H */

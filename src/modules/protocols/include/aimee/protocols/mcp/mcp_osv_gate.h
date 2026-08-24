/* mcp_osv_gate.h: the OSV malware gate for launching an MCP server.
 *
 * Extracted from mcp_client_registry.c so there is exactly ONE implementation.
 * Two callers now need it and they must not drift:
 *
 *   - the aimee.yaml client registry (mcp_client_registry.c), which has always
 *     refused to start a client whose package has malware advisories;
 *   - the plugin-module admission path (module_commands.c), where a Go module
 *     hosting one MCP server asks before spawning it.
 *
 * A second copy of this logic would be the more dangerous kind of bug: it would
 * pass every test while silently enforcing a different policy on one of the two
 * paths, and the path that got it wrong is the one that runs third-party code.
 *
 * Semantics, unchanged from the original:
 *   - the gate is inert unless mcp.osv.enabled
 *   - argv that does not resolve to a package-manager launch is allowed (there
 *     is nothing to look up)
 *   - an allowlisted ecosystem:name is allowed, and if it had advisories that
 *     is warned about and audited as allow_allowlisted
 *   - malware blocks ONLY when mcp.osv.enforce; otherwise it is a shadow-block
 *     (warned and audited, but allowed to run)
 *   - unknown (offline, cache miss, endpoint down) is allowed and warned
 * Every outcome is audited to the DB1 OSV audit table.
 */
#ifndef DEC_MCP_OSV_GATE_H
#define DEC_MCP_OSV_GATE_H 1

/* Returns 1 when |argv| must NOT be launched, 0 when it may be.
 *
 * |name| labels the launch in logs and audit rows (a config client name, or a
 * plugin module's instance name). |argc|/|argv| are the exact command that
 * would be executed; argv need not be NULL-terminated beyond argc. */
int mcp_osv_gate_blocks_argv(const char *name, int argc, const char *const argv[]);

#endif /* DEC_MCP_OSV_GATE_H */

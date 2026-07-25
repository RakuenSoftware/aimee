/* tool_completion_audit_bridge.h: install the tool-dispatch completion hook onto
 * the (D7-confined) audit bus. Server-only — the one object that links the tools
 * dispatcher to obs_bus. Call once at server startup, after obs_bus is available.
 * Records one OUTCOME row per completed tool dispatch (the half the pre-tool-check
 * governed-action row cannot see); identity + classified enums only, no content. */
#ifndef AIMEE_TOOL_COMPLETION_AUDIT_BRIDGE_H
#define AIMEE_TOOL_COMPLETION_AUDIT_BRIDGE_H 1

void tool_completion_audit_bridge_install(void);

#endif /* AIMEE_TOOL_COMPLETION_AUDIT_BRIDGE_H */

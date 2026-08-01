/* roundtable_activation.h: optional roundtable activation and runtime surfaces. */
#ifndef AIMEE_ROUNDTABLE_ACTIVATION_H
#define AIMEE_ROUNDTABLE_ACTIVATION_H

/* Resolve activation for direct engine/CLI callers. Explicit config wins over the
 * environment fallback; missing or invalid input fails closed. The resolution (and the
 * AIMEE_MODULE_ROUNDTABLE read) lives in the config module. */
int roundtable_module_enabled(void);
const char *roundtable_module_disabled_message(void);

/* Cache the server's startup-only activation state from the live config. Roundtable does
 * not support administrative hot toggling. */
void roundtable_runtime_configure(void);

/* Compose the optional roundtable implementation behind delegates core's panel
 * provider ABI. Startup-only; returns 1 enabled, 0 disabled, -1 on error. */
int roundtable_provider_configure(void);

/* Runtime composition predicates. Names not owned by roundtable pass through;
 * the exact/prefix ownership tables live only in roundtable_activation.c. */
int roundtable_operation_available(const char *operation);
int roundtable_tool_available(const char *tool);

#endif

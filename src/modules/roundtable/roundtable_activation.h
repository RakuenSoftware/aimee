/* roundtable_activation.h: optional roundtable activation and runtime surfaces. */
#ifndef AIMEE_ROUNDTABLE_ACTIVATION_H
#define AIMEE_ROUNDTABLE_ACTIVATION_H

/* Compose the optional roundtable implementation behind delegates core's panel
 * provider ABI. Startup-only; returns 1 enabled, 0 disabled, -1 on error. */
int roundtable_provider_configure(void);

int roundtable_tool_available(const char *tool);

#endif

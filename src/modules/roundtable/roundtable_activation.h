/* roundtable_activation.h: optional roundtable activation and runtime surfaces. */
#ifndef AIMEE_ROUNDTABLE_ACTIVATION_H
#define AIMEE_ROUNDTABLE_ACTIVATION_H

#include "config.h"

/* Resolve activation for direct engine/CLI callers. Explicit config wins over
 * the environment fallback; missing or invalid input fails closed. */
int roundtable_module_enabled(const config_t *cfg);
const char *roundtable_module_disabled_message(void);

/* Cache the server's startup-only activation state. Passing NULL disables the
 * module. Roundtable does not support administrative hot toggling. */
void roundtable_runtime_configure(const config_t *cfg);

/* Runtime composition predicates. Names not owned by roundtable pass through;
 * the exact/prefix ownership tables live only in roundtable_activation.c. */
int roundtable_operation_available(const char *operation);
int roundtable_tool_available(const char *tool);

#endif

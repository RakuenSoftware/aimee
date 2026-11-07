#ifndef AIMEE_SERVER_MODULE_NATIVE_TOOLS_H
#define AIMEE_SERVER_MODULE_NATIVE_TOOLS_H
#include <aimee/ir/aimee_ir.h>
/* Internal catalog ownership seams; none grants tool execution authority. */
int agent_tools_builtin_name_reserved(const char *name);
void agent_tools_module_native_collision(const char *name);
int agent_tools_module_native_snapshot(aimee_request_t *out);
#endif

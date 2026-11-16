#ifndef AIMEE_IR_HOST_BINDINGS_H
#define AIMEE_IR_HOST_BINDINGS_H
#include <aimee/ir/module_plan.h>

extern const aimee_ir_plan_binding_t server_ir_plan_bindings[];
extern const aimee_ir_plan_resource_t server_ir_plan_resources[];
void server_ir_plan_refuse(const char *kind, void *context);
/* -1 no owner, 0 selected owner disabled context, 1 enabled or selected failure.
 * Failure must reach the ordinary required-plan refusal, never skip admission. */
int server_ir_plan_enabled(const char *method, const char *operation, const char *value);
#endif

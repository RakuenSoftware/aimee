#ifndef AIMEE_IR_HOST_BINDINGS_H
#define AIMEE_IR_HOST_BINDINGS_H
#include <aimee/ir/module_plan.h>

extern const aimee_ir_plan_binding_t server_ir_plan_bindings[];
extern const aimee_ir_plan_resource_t server_ir_plan_resources[];
void server_ir_plan_refuse(const char *kind, void *context);
char *server_ir_plan_text(const char *method, const char *operation, const char *phase,
                          const char *query);
int server_ir_plan_enabled(const char *method, const char *operation, const char *value);
#endif

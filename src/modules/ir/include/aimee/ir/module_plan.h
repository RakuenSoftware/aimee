#ifndef AIMEE_IR_MODULE_PLAN_H
#define AIMEE_IR_MODULE_PLAN_H
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

/* The host explicitly supplies each effect a module plan may invoke. Callbacks
 * return owned JSON values; NULL means the effect was unavailable. */
typedef struct
{
   const char *name;
   cJSON *(*invoke)(const cJSON *args, void *context);
} aimee_ir_plan_binding_t;
typedef struct
{
   const char *name;
   const char *text;
} aimee_ir_plan_resource_t;
typedef struct
{
   const char *method;
   const char *operation;
   const char *phase;
   const char *provided_query;
   /* NULL-terminated resource names already inserted by this host assembly,
    * never inferred from caller text or model-supplied labels. */
   const char *const *provided_resources;
   const aimee_ir_plan_binding_t *bindings;   /* NULL-name terminated */
   const aimee_ir_plan_resource_t *resources; /* NULL-name terminated */
   void *context;
} aimee_ir_module_plan_t;

/* Request a host-only plan, validate it, then execute only declared effects and
 * IR edits. The stage returns 1 for an IR edit, 0 for no edit or unavailable plan.
 * Text execution returns an owned string, or NULL for an absent/empty result. */
int aimee_ir_stage_module_plan(aimee_request_t *request, void *configuration);
char *aimee_ir_module_plan_text(const aimee_ir_module_plan_t *configuration);
#endif

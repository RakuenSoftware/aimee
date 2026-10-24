#ifndef AIMEE_AGENT_DISPATCH_SCOPE_H
#define AIMEE_AGENT_DISPATCH_SCOPE_H
#include "agent_types.h"
/* Private synchronous resource lifetime, installed by the existing producer.
 * No request field, permission, model provenance or persistent registration. */
typedef struct
{
   int (*begin)(const agent_t *selected, void *context, char *error, size_t capacity);
   int (*end)(void *context, char *error, size_t capacity);
   void *context;
} agent_dispatch_scope_t;
/* Borrowed stack-owned scopes must outlive execution and be restored in order. */
const agent_dispatch_scope_t *agent_dispatch_scope_bind(const agent_dispatch_scope_t *scope);
const agent_dispatch_scope_t *agent_dispatch_scope_current(void);
#endif

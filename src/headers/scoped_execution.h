#ifndef AIMEE_SCOPED_EXECUTION_H
#define AIMEE_SCOPED_EXECUTION_H

/* A write may have taken effect, but its owner could not confirm completion. */
#define SCOPED_WRITE_OUTCOME_UNKNOWN (-2)

/* Explicit producer-local execution plumbing, not authority/config. NULL
 * retains the normal backend environment and wait behavior. */
typedef struct delegate_exec_scope
{
   char *const *environment;
   int replace_environment;
   int (*progress)(void *context);
   void *context;
} delegate_exec_scope_t;

#endif

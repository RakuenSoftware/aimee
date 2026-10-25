#ifndef SERVER_COMPUTE_INTERNAL_H
#define SERVER_COMPUTE_INTERNAL_H
#include <stddef.h>
#include "cJSON.h"
#include "server.h"
#include "delegate_ensemble.h"
#include <pthread.h>
#define DELEGATION_INPUT_TIMEOUT 60 /* seconds */
/* Parent/depth state is independent of canonical question controls. */
extern __thread char tl_parent_delegation_id[64];
extern __thread int tl_delegation_depth;
#endif /* SERVER_COMPUTE_INTERNAL_H */

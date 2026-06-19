/* Stub of agent_set_request_cancel for tests that link server_compute.c (the
 * worker registers an in-process cancel flag via this) but do not exercise
 * cooperative cancellation. The real implementation lives in
 * server/agent_config.c, which pulls in the full agent roster parser. */
#include "aimee.h"
#include "agent_config.h"

void agent_set_request_cancel(atomic_int *flag)
{
   (void)flag;
}

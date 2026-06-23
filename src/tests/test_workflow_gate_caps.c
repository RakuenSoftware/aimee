/* test_workflow_gate_caps.c -- the capability invariant the human-gate route
 * (POST /v1/workflow/items/<id>/gate, CAP_WORKFLOW_ADMIN) relies on: a mere
 * authenticated / delegate bearer must NOT carry the operator-level gate cap, so
 * it can never approve or reject an autonomous-workflow human gate. Verified at
 * compile time against the cap-set macros the route table consumes. */
#include <assert.h>
#include <stdio.h>

#include "server.h"

/* CAP_WORKFLOW_ADMIN is deliberately OUTSIDE CAPS_AUTHENTICATED (UDS / webchat-
 * admin / full-trust only), while CAP_DELEGATE is inside it. If either drifts,
 * a delegate bearer could drive a human gate -> fail the build, not production. */
_Static_assert((CAPS_AUTHENTICATED & CAP_WORKFLOW_ADMIN) == 0,
               "CAP_WORKFLOW_ADMIN must stay outside CAPS_AUTHENTICATED");
_Static_assert((CAPS_AUTHENTICATED & CAP_DELEGATE) != 0,
               "CAP_DELEGATE is expected inside CAPS_AUTHENTICATED");
_Static_assert(CAP_WORKFLOW_ADMIN != CAP_DELEGATE,
               "gate-admin and delegate must be distinct capabilities");

int main(void)
{
   printf("workflow-gate-caps: ok\n");
   return 0;
}

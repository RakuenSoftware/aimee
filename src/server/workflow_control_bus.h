#ifndef AIMEE_WORKFLOW_CONTROL_BUS_H
#define AIMEE_WORKFLOW_CONTROL_BUS_H

/* The workflows process's control stage. The kind is fixed by the process
 * contract at 4096 + ordinal*256 + stage; workflows is ordinal 20, so control
 * is the advance decision's successor rather than a free choice. */
#define AIMEE_WORKFLOWS_EVENT_CONTROL 9218u
#define AIMEE_WORKFLOWS_STAGE_CONTROL 2u

/* Dispatch one workflow control request over the event bus.
 *
 * Carries exactly what the private HTTP proxy carried. `body` need not be
 * NUL-terminated; it is taken by length. Returns the control plane's HTTP
 * status, or a transport status with a JSON error written into `resp`.
 */
int workflow_control_request(const char *method, const char *path, const char *query,
                             const char *body, int body_len, const char *principal,
                             int workflow_operator, char *resp, int resp_cap);

#endif

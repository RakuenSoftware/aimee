/* server_job_reply.h: keep delegate job replies inside the HTTP relay. */
#ifndef AIMEE_SERVER_JOB_REPLY_H
#define AIMEE_SERVER_JOB_REPLY_H

#include "cJSON.h"

/* Shrink a job reply (jobs.status, jobs.logs, delegate.status) until it fits the
 * HTTP relay: free text keeps its head and tail with the original size recorded
 * as <key>_bytes; if that is not enough, the transcript arrays are dropped and
 * their entry counts recorded as <key>_omitted. Applies to the reply and to a
 * nested "job" object. A reply that already fits is left unchanged. */
void server_job_reply_fit(cJSON *resp);

#endif

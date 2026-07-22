#ifndef DEC_DELEGATE_LAUNCH_H
#define DEC_DELEGATE_LAUNCH_H 1

#include <stddef.h>
#include "cJSON.h"

typedef struct
{
   int plan_id;
   int job_id;
   int tasks;
   int max_concurrent;
} delegate_launch_result_t;

int delegate_launch_coord_job(cJSON *plan, int max_concurrent, const char *cwd,
                              delegate_launch_result_t *out, char *errbuf, size_t errbuf_len);

#endif /* DEC_DELEGATE_LAUNCH_H */

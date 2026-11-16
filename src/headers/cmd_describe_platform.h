/* cmd_describe_platform.h: shared types and declarations for cmd_describe.c platform split. */
#ifndef DEC_CMD_DESCRIBE_PLATFORM_H
#define DEC_CMD_DESCRIBE_PLATFORM_H 1

#include "aimee.h"

#define DESCRIBE_MAX_PARALLEL_FALLBACK 3
#define DESCRIBE_MAX_PARALLEL_CAP      8

typedef struct
{
   char name[128];
   char path[MAX_PATH_LEN];
} describe_job_t;

/* Platform-specific parallel execution of describe/style jobs.
 * POSIX: forks up to max_parallel children at a time.
 * Windows: runs jobs serially.
 * Updates *described and *failed counts on output. */
void platform_describe_run_parallel(describe_job_t *jobs, int job_count, int max_parallel,
                                    int do_style, int attempt, int attempts, int *described,
                                    int *failed);

#endif /* DEC_CMD_DESCRIBE_PLATFORM_H */

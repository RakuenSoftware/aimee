#ifndef GIT_VERIFY_JOBS_H
#define GIT_VERIFY_JOBS_H

#include "server.h"
#include <pthread.h>

typedef struct verify_job
{
   int id;
   int active;
   volatile int cancel_requested;
   int passed;
   int failed;
   int total;
   char *output;
   char *verdict_json; /* structured (format=json) verdict, built at finalize; NULL until then */
   char file_hash[32];
   char session_id[SERVER_SESSION_ID_MAX];
   int lock_ready;
   pthread_mutex_t lock;
} verify_job_t;

verify_job_t *verify_job_get(int id);

verify_job_t *verify_job_alloc_for_session(const char *session_id, int *busy);
void verify_job_release(verify_job_t *job);
int verify_session_has_active_job(const char *session_id);
int verify_register_session_cancel(const char *session_id, volatile int *cancel_requested);
void verify_unregister_session_cancel(volatile int *cancel_requested);

#endif /* GIT_VERIFY_JOBS_H */

#ifndef DEC_KB_SERVICE_H
#define DEC_KB_SERVICE_H 1

#include <pthread.h>
#include <time.h>

#define KB_WORKER_MAX 8

typedef struct kb_service_ctx kb_service_ctx_t;

struct kb_service_ctx
{
   int listen_fd;
   int bg_listen_fd;
   volatile int running;
   long start_time;
   char socket_path[4096];
   char bg_socket_path[4096];

   /* Connection worker pool (accept threads; sized by kb.connection_workers) */
   int worker_count;
   pthread_t worker_threads[KB_WORKER_MAX];

   /* In-process KB ingest worker pool (DB2-direct; sized by kb.worker_count).
    * See kb_ingest_workers.c. */
   pthread_t ingest_threads[KB_WORKER_MAX];
   int ingest_count;
   pthread_mutex_t ingest_mu;
   pthread_cond_t ingest_cond;
   volatile int ingest_stop;
   pthread_t ingest_timer_thread;
   int ingest_timer_active;

   /* Background timer thread (maintenance) */
   pthread_t bg_timer_thread;
   int bg_timer_active;

   /* inotify watch thread (ingest file-change trigger) */
   pthread_t bg_watch_thread;
   int bg_watch_active;

   /* Last session RPC timestamp (epoch); updated on each kb_dispatch call.
    * Used by the reflection scheduler via kb_reflection_notify_session_rpc(). */
   long last_session_rpc_ts;
};

/* Owned copy of the current request's verifier-supplied authority and scope. */
struct cJSON;
struct cJSON *kb_service_command_context(void);

int kb_service_init(kb_service_ctx_t *ctx);
void kb_service_shutdown(kb_service_ctx_t *ctx);
void kb_worker_notify(kb_service_ctx_t *ctx);
char *kb_service_conn_slots_json(int configured);
char *kb_service_threads_json(kb_service_ctx_t *ctx);
char *kb_service_workers_json(kb_service_ctx_t *ctx);

#endif /* DEC_KB_SERVICE_H */

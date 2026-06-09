#ifndef DEC_COMPUTE_POOL_H
#define DEC_COMPUTE_POOL_H 1

#include <pthread.h>
#include <stdint.h>

#define COMPUTE_QUEUE_SIZE       128
#define COMPUTE_QUEUE_HIGH_PCT   80
#define COMPUTE_QUEUE_LOW_PCT    50
#define POOL_SLOT_DESCRIPTOR_MAX 128

typedef struct
{
   void (*fn)(void *arg);
   void *arg;
   int64_t deadline_ms; /* monotonic ms deadline, 0 = no timeout */
} work_item_t;

/* Pool job kinds. POOL_JOB_NONE means the slot is idle. The set is
 * extended as new caller types adopt slot tracking; callers default to
 * POOL_JOB_OTHER if no specific kind fits. */
typedef enum
{
   POOL_JOB_NONE = 0,
   POOL_JOB_DELEGATE,
   POOL_JOB_VERIFY,
   POOL_JOB_INGEST,
   POOL_JOB_TOOL,
   POOL_JOB_CHAT,
   POOL_JOB_KB_CURATOR,
   POOL_JOB_OTHER,
} pool_job_kind_t;

/* Per-worker slot state. Worker thread that owns the slot writes its own
 * fields under `mutex`; the JSON dumper reads them under the same lock. */
typedef struct
{
   int active; /* 1 while a job is running on this slot */
   pool_job_kind_t kind;
   char descriptor[POOL_SLOT_DESCRIPTOR_MAX];
   int64_t started_ms; /* CLOCK_MONOTONIC ms */
} pool_slot_t;

typedef struct
{
   pthread_t *threads;
   int thread_count;
   work_item_t queue[COMPUTE_QUEUE_SIZE];
   int queue_head;
   int queue_tail;
   int queue_count;
   pthread_mutex_t mutex;
   pthread_cond_t work_available;
   int shutdown;
   int high_water_logged; /* one-shot WARN per high→low cycle */
   pool_slot_t *slots;    /* thread_count entries; indexed by worker idx */
   void *worker_args;     /* opaque heap array of {pool, idx} for workers */
} compute_pool_t;

/* Initialize pool with given number of worker threads. Returns 0 on success. */
int compute_pool_init(compute_pool_t *pool, int num_threads);

/* Submit work to the pool. Returns 0 on success, -1 if queue is full. */
int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg);

/* Shut down pool: signal all workers, wait for completion. */
void compute_pool_shutdown(compute_pool_t *pool);

/* Worker-side: record this thread's current job. Must be called from a thread
 * spawned by compute_pool_init (i.e. a pool worker). No-op on other threads.
 * descriptor_fmt may be NULL or empty; truncated to POOL_SLOT_DESCRIPTOR_MAX-1.
 * Pairs with compute_pool_clear_job() called after the job finishes. */
void compute_pool_set_job(pool_job_kind_t kind, const char *descriptor_fmt, ...);

/* Worker-side: clear this thread's slot back to idle. Safe to call on a
 * non-pool thread (no-op). */
void compute_pool_clear_job(void);

/* Snapshot the pool's slots as a heap-allocated JSON array string. Caller
 * frees. Returns NULL on allocation failure. The JSON array shape is:
 *   [{"index":0,"active":true,"kind":"delegate","descriptor":"...",
 *     "elapsed_secs":12}, ...]
 */
char *compute_pool_slots_json(compute_pool_t *pool);

/* Return the configured worker thread count. */
int compute_pool_thread_count(const compute_pool_t *pool);

/* Map a pool_job_kind_t to a stable lowercase string, e.g. "delegate". */
const char *pool_job_kind_name(pool_job_kind_t kind);

/* ---- Secondary-pool registry ----
 *
 * The primary aimee-server pool (ctx->pool) is not the only compute_pool_t
 * the server hosts. Some subsystems still allocate ephemeral pools for the
 * duration of a job (e.g. a verify run). The registry lets those pools
 * advertise their slots through `aimee workers` for as long as they live.
 *
 * Each pool gets a short symbolic name ("verify", etc.) used as the block
 * heading. The registry holds a small fixed number of entries; if the
 * registry is full, registration is a silent no-op and only the primary
 * pool is reported. Concurrent registrations of the same `pool` overwrite
 * the previous entry in place.
 */
#define COMPUTE_POOL_REGISTRY_MAX 8
#define COMPUTE_POOL_NAME_MAX     32

void compute_pool_register_secondary(compute_pool_t *pool, const char *name);
void compute_pool_unregister_secondary(compute_pool_t *pool);

/* JSON array shape:
 *   [{"name":"verify","configured":4,"slots":[ ... ]}, ...]
 * Heap-allocated; caller frees. Returns NULL on failure or "[]" when no
 * pools are registered. */
char *compute_pool_secondary_pools_json(void);

#include "compute_concurrency.h"

#endif /* DEC_COMPUTE_POOL_H */

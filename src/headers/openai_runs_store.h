/* openai_runs_store.h: in-process *live* store for OpenAI-style /v1/runs records.
 *
 * A run is created in `queued`, transitions to `in_progress`, then to a terminal
 * status (`completed` / `cancelled` / `failed`). While live, a record carries:
 *   - a GET snapshot: the latest `run` JSON object returned by GET /v1/runs/{id};
 *   - an append-only event buffer of SSE frames (name + data JSON) that
 *     GET /v1/runs/{id}/events streams live to subscribers;
 *   - a cancel-requested flag that POST /v1/runs/{id}/stop sets and the run
 *     worker polls between steps.
 *
 * The background run worker (see openai_chat.c) produces events and status
 * transitions on its own thread; subscribers block in openai_runs_store_wait()
 * on the listener thread until new events or a terminal status appear. All
 * functions are thread-safe (one mutex + condition variable per store).
 *
 * Bounded (OPENAI_RUNS_STORE_MAX concurrent records, reused oldest-first) and
 * not durable across restarts. */
#ifndef DEC_OPENAI_RUNS_STORE_H
#define DEC_OPENAI_RUNS_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run lifecycle status. Ordered: queued < in_progress < terminal. */
   typedef enum
   {
      OPENAI_RUN_QUEUED = 0,
      OPENAI_RUN_IN_PROGRESS,
      OPENAI_RUN_COMPLETED,
      OPENAI_RUN_CANCELLED,
      OPENAI_RUN_FAILED,
   } openai_run_status_t;

   /* Canonical lowercase wire string for a status ("queued", "in_progress",
    * "completed", "cancelled", "failed"). Stable; safe for response bodies. */
   const char *openai_run_status_str(openai_run_status_t status);

   /* True (non-zero) if status is terminal (completed / cancelled / failed). */
   int openai_run_status_terminal(openai_run_status_t status);

   /* Stable, process-lifetime generation token embedded in newly-created op-run
    * ids. It lets a replacement server distinguish an interrupted run from an
    * arbitrary unknown id without retaining the old process's live store. */
   const char *openai_runs_store_generation(void);

   typedef enum
   {
      OPENAI_RUNS_MISSING_UNKNOWN = 0,
      OPENAI_RUNS_MISSING_INTERRUPTED,
      OPENAI_RUNS_MISSING_EVICTED,
   } openai_runs_missing_t;

   /* Classify a missing op-run id. A prior generation is interrupted; a valid
    * current-generation id was evicted from the bounded live store. Legacy
    * timestamp-based op-run ids are treated as interrupted after upgrade. */
   openai_runs_missing_t openai_runs_store_classify_missing(const char *run_id);

   /* Create a new live record for run_id in OPENAI_RUN_QUEUED with run_json as
    * the initial GET snapshot. Returns 1 on success, 0 if run_id/run_json is
    * empty or a record with run_id already exists. */
   int openai_runs_store_create(const char *run_id, const char *run_json);

   /* Replace the GET snapshot (the JSON returned by GET /v1/runs/{id}) without
    * changing status. The worker calls this when it has reshaped the run object
    * (e.g. on the queued->in_progress transition). No-op if unknown. */
   void openai_runs_store_update_json(const char *run_id, const char *run_json);

   /* Set the run's status and broadcast to waiters. Intended for the
    * non-terminal queued->in_progress transition; use finalize() for terminal
    * states so the snapshot and terminal flag move atomically. No-op if unknown
    * or already terminal. */
   void openai_runs_store_set_status(const char *run_id, openai_run_status_t status);

   /* Append an SSE event (event_name + data_json, both copied) to the live
    * buffer and broadcast to waiters. No-op if the run is unknown or already
    * terminal. data_json may be up to OPENAI_RUNS_EVENT_MAX bytes; longer data
    * is truncated. */
   void openai_runs_store_append_event(const char *run_id, const char *event_name,
                                       const char *data_json);

   /* Mark the run terminal: replace the GET snapshot with final_run_json, set
    * the terminal status, set the terminal flag, and wake all waiters. After
    * this, append_event/set_status are no-ops. No-op if unknown. */
   void openai_runs_store_finalize(const char *run_id, openai_run_status_t status,
                                   const char *final_run_json);

   /* Set the cancel-requested flag and broadcast (wakes the worker if it is
    * waiting, and any subscribers). Returns 1 if the run existed and was not
    * already terminal, 0 otherwise. */
   int openai_runs_store_request_cancel(const char *run_id);

   /* True if cancellation has been requested for run_id (the run worker polls
    * this between steps). 0 if unknown. */
   int openai_runs_store_cancel_requested(const char *run_id);

   /* Read the current status into *out. Returns 1 if the run exists, 0
    * otherwise (and leaves *out untouched). */
   int openai_runs_store_status(const char *run_id, openai_run_status_t *out);

   /* Copy the current GET snapshot into out[out_n] (NUL-terminated, truncated
    * if needed). Returns 1 if found, 0 otherwise (and sets out[0]='\0' when
    * out/out_n are valid). */
   int openai_runs_store_get(const char *run_id, char *out, size_t out_n);

   /* Result of openai_runs_store_wait(). */
   typedef enum
   {
      OPENAI_RUNS_WAIT_EVENT = 0, /* an event was copied out; *cursor advanced */
      OPENAI_RUNS_WAIT_TERMINAL,  /* no more events and the run is terminal     */
      OPENAI_RUNS_WAIT_TIMEOUT,   /* timeout elapsed with nothing new buffered  */
      OPENAI_RUNS_WAIT_GONE,      /* run_id is unknown                          */
   } openai_runs_wait_t;

   /* Subscribe to a run's live event stream. *cursor is the caller's position
    * (start at 0). Blocks up to timeout_ms (<=0 = return immediately if nothing
    * pending) until an event at index >= *cursor is buffered or the run becomes
    * terminal.
    *
    *   - OPENAI_RUNS_WAIT_EVENT: copies the next event's name into
    *     ev_name[ev_name_n] and data into data[data_n] (both NUL-terminated,
    *     truncated to fit) and advances *cursor past it. Drain remaining
    *     buffered events by calling again before re-blocking.
    *   - OPENAI_RUNS_WAIT_TERMINAL: all buffered events at/after *cursor are
    *     drained and the run reached a terminal status. Stop streaming.
    *   - OPENAI_RUNS_WAIT_TIMEOUT: nothing new within timeout_ms; the caller
    *     should check for client disconnect, then call again.
    *   - OPENAI_RUNS_WAIT_GONE: run_id unknown.
    *
    * Terminal is reported only after all preceding events are consumed, so an
    * event and the terminal signal are never lost or reordered. */
   openai_runs_wait_t openai_runs_store_wait(const char *run_id, size_t *cursor, int timeout_ms,
                                             char *ev_name, size_t ev_name_n, char *data,
                                             size_t data_n);

   /* Drop all records (test helper / shutdown). */
   void openai_runs_store_reset(void);

/* Maximum stored size of a single event's data JSON (terminal frames carry the
 * full run object). */
#define OPENAI_RUNS_EVENT_MAX (256 * 1024)

#ifdef __cplusplus
}
#endif

#endif /* DEC_OPENAI_RUNS_STORE_H */

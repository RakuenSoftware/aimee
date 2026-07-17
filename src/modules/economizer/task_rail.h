/* task_rail.h: portable plan state machine (fold §8, P5).
 *
 * A small, dependency-light execution spine that lives OUTSIDE the prompt: the
 * agent's plan as a locked list of steps with state + evidence, serializable to
 * JSON so it survives folds, epoch rebirths, and session boundaries (persist to
 * DB1 checkpoints.snapshot). Pure: cJSON only, no DB, no clock. Deterministic
 * serialization (stable key/step order). */
#ifndef DEC_TASK_RAIL_H
#define DEC_TASK_RAIL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      TASK_STEP_PENDING = 0,
      TASK_STEP_RESERVED, /* claimed / in flight */
      TASK_STEP_DONE
   } task_step_state_t;

   typedef struct
   {
      char *title;
      char *evidence; /* set when DONE (handle/note); may be NULL */
      task_step_state_t state;
   } task_step_t;

   typedef struct
   {
      char *objective;
      int locked; /* 1 once started — the step list is fixed */
      task_step_t *steps;
      size_t count;
      size_t cap;
   } task_rail_t;

   void task_rail_init(task_rail_t *r);
   void task_rail_free(task_rail_t *r);

   /* Start (and lock) a rail with an objective and `n` step titles. Replaces any
    * prior state. Returns 0 on success, -1 on bad args. */
   int task_rail_start(task_rail_t *r, const char *objective, const char *const *titles, size_t n);

   /* Reserve a step (PENDING -> RESERVED). Returns 0, -1 on bad index/state. */
   int task_rail_reserve(task_rail_t *r, size_t idx);

   /* Acknowledge a step done (PENDING or RESERVED -> DONE) with optional evidence.
    * Returns 0; -1 on bad index or if duplicating evidence fails (step unchanged). */
   int task_rail_ack(task_rail_t *r, size_t idx, const char *evidence);

   /* Index of the first not-DONE step — i.e. the next unfinished step, which
    * INCLUDES a RESERVED (in-flight) step — or -1 if all done. */
   long task_rail_next(const task_rail_t *r);

   /* Count of DONE steps. */
   size_t task_rail_done_count(const task_rail_t *r);

   /* Serialize to a freshly malloc'd JSON string (caller frees). Deterministic. */
   char *task_rail_serialize(const task_rail_t *r);

   /* Restore from JSON. Validates and builds into a temporary, swapping into *r
    * only on complete success — so on -1 (malformed JSON, non-object root,
    * non-array steps, or OOM) the caller's existing rail is left UNCHANGED. An
    * out-of-range step state normalizes to PENDING. Returns 0 / -1. */
   int task_rail_restore(task_rail_t *r, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* DEC_TASK_RAIL_H */

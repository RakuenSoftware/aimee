/* db2/tasks.h: task graph — DB2 subsystem.
 *
 * Owns two tables:
 *   tasks         — id, parent_id, title, state, confidence, session_id,
 *                   success_criteria, ... timestamps
 *   task_edges    — source_id, target_id, relation
 *
 * Typed API; SQL is encapsulated in db2/tasks.c. */
#ifndef DEC_DB2_TASKS_H
#define DEC_DB2_TASKS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      int64_t parent_id; /* 0 if no parent */
      char title[256];
      char state[16];
      double confidence;
      char created_at[32];
      char updated_at[32];
      char session_id[128];
   } aimee_task_t;

   typedef struct
   {
      int64_t id;
      int64_t source_id;
      int64_t target_id;
      char relation[32];
   } task_edge_t;

   /* Task CRUD */
   int db2_task_create(const char *title, const char *session_id, int64_t parent_id,
                       aimee_task_t *out);
   int db2_task_get(int64_t id, aimee_task_t *out);
   int db2_task_update_state(int64_t id, const char *state);
   int db2_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out,
                     int max);
   int db2_task_delete(int64_t id);

   /* Task edges */
   int db2_task_add_edge(int64_t source, int64_t target, const char *relation);
   int db2_task_get_edges(int64_t task_id, task_edge_t *out, int max);
   int db2_task_get_subtasks(int64_t parent_id, aimee_task_t *out, int max);

   /* Get the most recent in_progress task for a session, or 0. */
   int64_t db2_task_get_active(const char *session_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TASKS_H */

/* db1/db1_trigger.h: trigger_runs table — event-triggered autopilot queue. */
#pragma once
#include <stddef.h>

typedef struct
{
   char id[64];
   char source[128];
   char event[256];
   char task[1024];
   char workspace[256];
   char metadata[2048];
   char pipeline_id[64];
   char status[32];
   char queued_at[32];
   char started_at[32];
   char finished_at[32];
   char error[512];
} db1_trigger_run_t;

int db1_trigger_insert(const char *id, const char *source, const char *event, const char *task,
                       const char *workspace, const char *metadata);
int db1_trigger_status_set(const char *id, const char *status, const char *pipeline_id,
                           const char *error);
int db1_trigger_get(const char *id, db1_trigger_run_t *out);
/* Returns malloc'd JSON array of db1_trigger_run_t objects, or NULL on error. */
char *db1_trigger_list_json(const char *status_filter);

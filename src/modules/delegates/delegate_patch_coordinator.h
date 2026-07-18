/* delegate_patch_coordinator.h: read-only patch-state reporting for coord jobs. */
#ifndef DEC_DELEGATE_PATCH_COORDINATOR_H
#define DEC_DELEGATE_PATCH_COORDINATOR_H 1

#include "coord_jobs.h"
#include "cJSON.h"

#define DELEGATE_PATCH_STATE_LEN 32
#define DELEGATE_PATCH_NOTE_LEN  256
#define DELEGATE_PATCH_FILE_LEN  256
#define DELEGATE_PATCH_MAX_FILES 32

typedef struct
{
   int task_id;
   int step_id;
   char task_status[DB1_COORD_STATUS_LEN];
   char patch_state[DELEGATE_PATCH_STATE_LEN];
   char handoff_status[DELEGATE_PATCH_STATE_LEN];
   int handoff_valid;
   int changed_files_count;
   int passed_tests;
   int outside_ownership_count;
   int overlap_task_id;
   int stale_base;
   int supervisor_actions;
   char note[DELEGATE_PATCH_NOTE_LEN];
} delegate_patch_task_report_t;

typedef struct
{
   int task_count;
   int implementation_packets;
   int planned;
   int running;
   int returned;
   int verified;
   int reviewable;
   int accepted;
   int failed;
   int needs_supervisor;
   int invalid_handoffs;
   int outside_ownership_touches;
   int patch_overlaps;
   int stale_worktrees;
   int focused_tests_passed;
   int reviewer_packets;
   int reviewer_blocking_findings;
   int reviewer_owner_packet_routes;
   char reviewer_status[DELEGATE_PATCH_STATE_LEN];
   char recommended_next_command[64];
   delegate_patch_task_report_t tasks[DB1_COORD_MAX_TASKS];
} delegate_patch_report_t;

void delegate_patch_coordinator_build_report(const db1_coord_job_t *job,
                                             const db1_coord_task_t *tasks, int task_count,
                                             delegate_patch_report_t *out);
void delegate_patch_coordinator_add_json(cJSON *obj, const delegate_patch_report_t *report);
const char *delegate_patch_coordinator_brief(const delegate_patch_report_t *report, char *buf,
                                             size_t cap);

#endif /* DEC_DELEGATE_PATCH_COORDINATOR_H */

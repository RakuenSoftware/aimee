/* db1/roadmap_runtime.h: DB1 CRUD helpers for spec-driven roadmap dispatch. */

#ifndef DEC_DB1_ROADMAP_RUNTIME_H
#define DEC_DB1_ROADMAP_RUNTIME_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RDM_ID_LEN     64
#define RDM_STATUS_LEN 32
#define RDM_PHASE_LEN  32
#define RDM_POLICY_LEN 32
#define RDM_PATH_LEN   512
#define RDM_RESULT_LEN 2048
#define RDM_ERROR_LEN  512
#define RDM_TS_LEN     32

   typedef struct
   {
      int id;
      char roadmap_id[RDM_ID_LEN];
      char status[RDM_STATUS_LEN];
      char phase[RDM_PHASE_LEN];
      char token_profile[RDM_STATUS_LEN];
      int require_slice_discussion;
      int budget_ceiling_tokens;
      char exit_reason[RDM_ERROR_LEN];
      char created_at[RDM_TS_LEN];
      char updated_at[RDM_TS_LEN];
   } rdm_dispatch_t;

   typedef struct
   {
      int id;
      char roadmap_id[RDM_ID_LEN];
      char unit_id[RDM_ID_LEN];
      char level[RDM_PHASE_LEN];
      char state[RDM_STATUS_LEN];
      char tool_policy_mode[RDM_POLICY_LEN];
      char claimed_by[64];
      char claimed_at[RDM_TS_LEN];
      char heartbeat_at[RDM_TS_LEN];
      int verify_attempts;
      int dispatch_attempts;
      char worktree_path[RDM_PATH_LEN];
      int coord_job_id;
      char result[RDM_RESULT_LEN];
      char error[RDM_ERROR_LEN];
      char created_at[RDM_TS_LEN];
      char updated_at[RDM_TS_LEN];
   } rdm_unit_dispatch_t;

   int rdm_dispatch_upsert(const char *roadmap_id, const char *token_profile,
                           int require_slice_discussion, int budget_ceiling_tokens);
   int rdm_dispatch_get(const char *roadmap_id, rdm_dispatch_t *out);
   int rdm_dispatch_set_status(const char *roadmap_id, const char *status, const char *exit_reason);
   int rdm_dispatch_set_phase(const char *roadmap_id, const char *phase);

   int rdm_unit_ensure(const char *roadmap_id, const char *unit_id, const char *level,
                       const char *tool_policy_mode);
   int rdm_unit_get(const char *roadmap_id, const char *unit_id, rdm_unit_dispatch_t *out);
   int rdm_unit_set_state(const char *roadmap_id, const char *unit_id, const char *state);
   int rdm_unit_claim(const char *roadmap_id, const char *unit_id, const char *owner,
                      const char *worktree_path);
   int rdm_unit_heartbeat(const char *roadmap_id, const char *unit_id);
   int rdm_unit_finish(const char *roadmap_id, const char *unit_id, const char *state,
                       const char *result, const char *error);
   int rdm_unit_set_coord_job(const char *roadmap_id, const char *unit_id, int coord_job_id);
   int rdm_unit_increment_verify_attempts(const char *roadmap_id, const char *unit_id);
   int rdm_unit_select_next(const char *roadmap_id, char *out_unit_id, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_ROADMAP_RUNTIME_H */
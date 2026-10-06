/* kb_store/mining.h: KB_STORE substrate for aimee-kb continuous mining. */
#ifndef DEC_KB_STORE_MINING_H
#define DEC_KB_STORE_MINING_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      char id[64];
      char last_run_at[32];
      int64_t hwm;
      int interval_s;
      int enabled;
      char last_error[512];
   } kb_store_mining_job_row_t;

   typedef struct
   {
      int64_t source_event_id;
      char session_id[128];
      char event_type[32];
      char role[64];
      char failure_mode[128];
      char scope_kind[32];
      char scope_id[128];
      char task_family[128];
      char action_sequence[512];
      char error_signature[256];
      char environment[256];
      char preconditions[512];
      char outcome[32];
      char recovery_action[512];
      char payload_json[4096];
      char embedding[2048];
      char cluster_key[128];
   } kb_store_mining_event_t;

   int kb_store_mining_seed_job_defaults(void);
   int kb_store_mining_job_get(const char *id, kb_store_mining_job_row_t *out);
   int kb_store_mining_job_complete(const char *id, int64_t hwm, const char *error);
   int kb_store_mining_job_try_lock(const char *id);
   void kb_store_mining_job_unlock(const char *id);
   int kb_store_mining_event_upsert(const kb_store_mining_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_MINING_H */

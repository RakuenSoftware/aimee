/* db2/mining.h: DB2 substrate for aimee-kb continuous mining. */
#ifndef DEC_DB2_MINING_H
#define DEC_DB2_MINING_H 1

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
   } db2_mining_job_row_t;

   typedef struct
   {
      int64_t source_event_id;
      char session_id[128];
      char event_type[32];
      char role[64];
      char failure_mode[128];
      char payload_json[4096];
      char embedding[2048];
      char cluster_key[128];
   } db2_mining_event_t;

   int db2_mining_seed_job_defaults(void);
   int db2_mining_job_get(const char *id, db2_mining_job_row_t *out);
   int db2_mining_job_complete(const char *id, int64_t hwm, const char *error);
   int db2_mining_job_try_lock(const char *id);
   void db2_mining_job_unlock(const char *id);
   int db2_mining_event_upsert(const db2_mining_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MINING_H */

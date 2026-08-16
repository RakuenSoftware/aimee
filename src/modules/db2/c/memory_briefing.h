/* db2/memory_briefing.h: SELECT primitives that feed the memory_briefing
 * JSON bundle. DB2-owned. The orchestrator (memory_advanced.c)
 * converts these typed rows into cJSON. */
#ifndef DEC_DB2_MEMORY_BRIEFING_H
#define DEC_DB2_MEMORY_BRIEFING_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t memory_id;
      char tier[8];
      char kind[16];
      char key[256];
      char text[2048];
      double confidence;
      double evidence_strength;
      int observation_count;
      int use_count;
      char last_seen_at[32];
   } db2_memory_briefing_fact_t;

   typedef struct
   {
      char session_id[64];
      char summary[2048];
      char reference_time[32];
      char created_at[32];
   } db2_memory_briefing_activity_t;

   typedef struct
   {
      char name[256];
      int mentions;
      char last_seen[32];
   } db2_memory_briefing_entity_t;

   /* L2+ memories ranked by (confidence + evidence_strength) DESC,
    * obs DESC, use_count DESC, id DESC. Returns count written. */
   int db2_memory_briefing_list_key_facts(db2_memory_briefing_fact_t *out, int max);

   /* Most recent episode card per source_session, newest first. */
   int db2_memory_briefing_list_recent_activity(db2_memory_briefing_activity_t *out, int max);

   /* Top-mentioned entities over memories touched in the last 30 days
    * (or never-touched fresh writes). */
   int db2_memory_briefing_list_active_entities(db2_memory_briefing_entity_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_BRIEFING_H */

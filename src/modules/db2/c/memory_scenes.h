/* memory_scenes.h: DB2 domain API for the scene-clustering tables.
 *
 * Callers pass pure domain values; backend access stays private to
 * src/modules/db2/c/. */
#ifndef DEC_DB2_MEMORY_SCENES_H
#define DEC_DB2_MEMORY_SCENES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Row shape for db2_memory_scenes_list_recent. */
   typedef struct
   {
      int64_t id;
      char workspace_id[128];
      int turn_count;
      char created_at[32];
   } db2_memory_scene_row_t;

   /* Row shape for db2_memory_scene_members. */
   typedef struct
   {
      int64_t memory_id;
      char key[256];
      double membership_strength;
   } db2_memory_scene_member_t;

   /* Fetch the most recent scenes (up to `max`, LIMIT 100 in SQL).
    * Returns the count written, or -1 on error. */
   int db2_memory_scenes_list_recent(db2_memory_scene_row_t *rows, int max);

   /* Fetch members of one scene ordered by descending membership.
    * Returns the count written, or -1 on error. */
   int db2_memory_scene_members(int64_t scene_id, db2_memory_scene_member_t *rows, int max);

   /* Row shape for db2_memory_scene_memberships_for_memory. */
   typedef struct
   {
      int64_t scene_id;
      double membership_strength;
   } db2_memory_scene_membership_t;

   /* Fetch the (scene_id, membership_strength) pairs for one memory. Used
    * by the scene-cluster scorer. Returns count written. */
   int db2_memory_scene_memberships_for_memory(int64_t memory_id,
                                               db2_memory_scene_membership_t *rows, int max);

   /* Returns 1 iff memory_scene_members has a row matching (memory_id,
    * scene_id), 0 otherwise. */
   int db2_memory_scene_member_exists(int64_t memory_id, int64_t scene_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_SCENES_H */

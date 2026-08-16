/* lessons_capture.c: see lessons_capture.h. The cite-emit hook substrate. */
#include "lessons_capture.h"
#include "modules/db2/c/lessons.h"

int lessons_capture_turn(lessons_cite_tracker_t *tracker, const char *session_id,
                         const char *turn_id, int turn, const char *project_id,
                         int64_t generation_id, const char *const *node_ids, int n_nodes)
{
   if (!tracker || !node_ids || n_nodes < 0)
      return -1;

   int recorded = 0;
   for (int i = 0; i < n_nodes; i++)
   {
      const char *node = node_ids[i];
      if (!node || !node[0])
         continue;
      /* The auto-`useful` proxy: a re-citation within N turns. LESSONS_AUTO_USEFUL_TURNS
       * is the default window (config-overridable by the caller in a later slice). */
      if (!lessons_cite_observe(tracker, node, turn, LESSONS_AUTO_USEFUL_TURNS))
         continue;

      /* Cite-again fired → this source earned an auto-`useful`. Agent-sourced +
       * unconfirmed: it influences nothing until S3c's actor model accepts it, per
       * the correction-authority model. Best-effort: a DB failure is not fatal. */
      int64_t oid = db2_lessons_record_outcome(session_id, turn_id, project_id, generation_id,
                                               "useful", "", "", "", "agent", 0);
      /* Count only a COMPLETE record (outcome + its citation), so `recorded` never
       * overstates what is actually visible in the ledger. */
      if (oid > 0 && db2_lessons_record_citation(oid, node, "useful") == 0)
         recorded++;
   }
   return recorded;
}

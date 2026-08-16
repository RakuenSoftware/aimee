/* lessons_session_capture.c: server-driven LIVE cite-capture (graph-feedback §3).
 *
 * A bounded, mutex-guarded map of session_id → {per-session cite-tracker, turn
 * counter}. Each observe call is one turn for that session; a node re-cited within
 * the auto-useful window records an agent-sourced, UNCONFIRMED 'useful' outcome in
 * the ledger (inert until a user/reviewer confirms, per the §3 authority model).
 * Best-effort: the in-memory tracker work happens under the lock; the DB writes for
 * the nodes that fired happen OUTSIDE the lock so DB I/O never serializes callers. */
#include "lessons_session_capture.h"

#include "modules/db2/c/lessons.h"
#include "lessons_cite_tracker.h" /* tracker + LESSONS_AUTO_USEFUL_TURNS + LESSONS_NODE_MAX */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define LSC_MAX_SESSIONS 256
#define LSC_MAX_FIRED    64 /* per-call cap on nodes that trigger a DB write */

typedef struct
{
   char session_id[128];
   lessons_cite_tracker_t tracker;
   int turn;
   unsigned long last_used;
   int in_use;
} lsc_slot_t;

static lsc_slot_t g_slots[LSC_MAX_SESSIONS];
static unsigned long g_clock = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int lessons_session_observe(const char *project, long long generation_id, const char *session_id,
                            const char *const *node_ids, int n_nodes)
{
   if (!project || !project[0] || !session_id || !session_id[0] || !node_ids || n_nodes < 0)
      return -1;

   char fired[LSC_MAX_FIRED][LESSONS_NODE_MAX];
   int n_fired = 0;
   int turn = 0;

   pthread_mutex_lock(&g_lock);
   /* Slot resolution: (1) match this session; else (2) a free slot; else (3) LRU-evict. */
   int idx = -1;
   for (int i = 0; i < LSC_MAX_SESSIONS; i++)
      if (g_slots[i].in_use && strcmp(g_slots[i].session_id, session_id) == 0)
      {
         idx = i;
         break;
      }
   if (idx < 0)
      for (int i = 0; i < LSC_MAX_SESSIONS; i++)
         if (!g_slots[i].in_use)
         {
            idx = i;
            break;
         }
   if (idx < 0)
   {
      idx = 0;
      for (int i = 1; i < LSC_MAX_SESSIONS; i++)
         if (g_slots[i].last_used < g_slots[idx].last_used)
            idx = i;
   }
   if (!g_slots[idx].in_use || strcmp(g_slots[idx].session_id, session_id) != 0)
   {
      memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
      snprintf(g_slots[idx].session_id, sizeof(g_slots[idx].session_id), "%s", session_id);
      lessons_cite_tracker_init(&g_slots[idx].tracker);
      g_slots[idx].in_use = 1;
   }
   g_slots[idx].turn++;
   turn = g_slots[idx].turn;
   g_slots[idx].last_used = ++g_clock;
   for (int i = 0; i < n_nodes; i++)
   {
      const char *node = node_ids[i];
      if (!node || !node[0])
         continue;
      if (lessons_cite_observe(&g_slots[idx].tracker, node, turn, LESSONS_AUTO_USEFUL_TURNS) &&
          n_fired < LSC_MAX_FIRED)
         snprintf(fired[n_fired++], LESSONS_NODE_MAX, "%s", node);
   }
   pthread_mutex_unlock(&g_lock);

   /* DB writes for the nodes that fired the auto-useful proxy — outside the lock. */
   int recorded = 0;
   for (int i = 0; i < n_fired; i++)
   {
      int64_t oid = db2_lessons_record_outcome(session_id, "", project, generation_id, "useful", "",
                                               "", "", "agent", 0);
      if (oid > 0 && db2_lessons_record_citation(oid, fired[i], "useful") == 0)
         recorded++;
   }
   return recorded;
}

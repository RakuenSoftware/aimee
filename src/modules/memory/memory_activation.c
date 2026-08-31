/* memory_activation.c: per-unit retrieval hysteresis.
 * See memory_activation.h for the gate order and why cooldown beats sticky. */

#include "memory_activation.h"

#include "db1_optional.h"
#include "db1_client/caches.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local memory_activation_t g_last_activation;
static _Thread_local char g_last_activation_session[128];
static _Thread_local int g_last_activation_valid;

void memory_activation_load(memory_activation_t *out, const char *session_id)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   /* A failed or deliberately skipped load must not leave the preceding
    * successful snapshot available to an explain call for the same session. */
   g_last_activation_valid = 0;
   g_last_activation_session[0] = '\0';
   if (!session_id || !session_id[0] || !db1_context_snapshot_activation)
      return; /* not loaded -> every predicate answers "no opinion" */

   char rows[MEMORY_ACTIVATION_MAX_ROWS][DB1_CONTEXT_ACTIVATION_ROW_LEN];
   int n = db1_context_snapshot_activation(session_id, rows, MEMORY_ACTIVATION_MAX_ROWS);
   if (n < 0)
      return; /* store unreadable: fail open rather than withhold evidence */

   for (int i = 0; i < n && out->count < MEMORY_ACTIVATION_MAX_ROWS; i++)
   {
      char *end = NULL;
      long long mid = strtoll(rows[i], &end, 10);
      if (mid < 0 || !end)
         continue;
      long long turn = strtoll(end, NULL, 10);
      if (turn < 0)
         continue;
      if (mid == 0)
      {
         out->current_turn = (int64_t)turn;
         continue;
      }
      out->rows[out->count].memory_id = (int64_t)mid;
      out->rows[out->count].last_turn = (int64_t)turn;
      out->count++;
   }
   /* The store advances this even when no candidate is injected. Deriving it
    * from the highest injection row made empty-recall turns disappear, so delay
    * and cooldown measured injections rather than conversation turns. */
   out->loaded = out->current_turn > 0;
   g_last_activation = *out;
   snprintf(g_last_activation_session, sizeof(g_last_activation_session), "%s", session_id);
   g_last_activation_valid = 1;
}

int memory_activation_last_loaded(const char *session_id, memory_activation_t *out)
{
   if (!out || !session_id || !session_id[0] || !g_last_activation_valid ||
       strcmp(session_id, g_last_activation_session) != 0)
      return 0;
   *out = g_last_activation;
   return 1;
}

int64_t memory_activation_last_turn(const memory_activation_t *act, int64_t memory_id)
{
   if (!act || !act->loaded || memory_id <= 0)
      return 0;
   for (int i = 0; i < act->count; i++)
      if (act->rows[i].memory_id == memory_id)
         return act->rows[i].last_turn;
   return 0;
}

int memory_activation_in_cooldown(const memory_activation_t *act, int64_t memory_id,
                                  int cooldown_turns)
{
   if (!act || !act->loaded || cooldown_turns <= 0)
      return 0; /* no opinion -> the caller's relevance decision stands */
   int64_t last = memory_activation_last_turn(act, memory_id);
   if (last <= 0)
      return 0; /* never fired here, so nothing to cool down from */
   /* A malformed/legacy zero-turn row is handled above as "never fired",
    * leaving the unit eligible rather than muting it on a value that never
    * identified a real conversation turn. */
   return (act->current_turn - last) <= (int64_t)cooldown_turns;
}

int memory_activation_is_sticky(const memory_activation_t *act, int64_t memory_id, int sticky_turns)
{
   if (!act || !act->loaded || sticky_turns <= 0)
      return 0;
   int64_t last = memory_activation_last_turn(act, memory_id);
   if (last <= 0)
      return 0;
   return (act->current_turn - last) <= (int64_t)sticky_turns;
}

int memory_activation_is_delayed(const memory_activation_t *act, int delay_turns)
{
   if (!act || !act->loaded || delay_turns <= 0)
      return 0;
   return act->current_turn <= (int64_t)delay_turns;
}

void memory_activation_record(const memory_activation_t *act, const char *session_id,
                              int64_t memory_id, double score)
{
   if (!session_id || !session_id[0] || memory_id <= 0)
      return;
   int64_t turn = (act && act->loaded) ? act->current_turn : 0;
   if (turn > 0 && db1_context_snapshot_insert_turn)
   {
      (void)db1_context_snapshot_insert_turn(session_id, memory_id, score, turn);
      return;
   }
   /* Store unavailable: fail open and omit activation history. Never fall back
    * to context_snapshot_insert: that table drives effectiveness sampling, and
    * an injection is not evidence that the injected memory was useful. */
}

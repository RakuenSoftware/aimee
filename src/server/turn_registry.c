/* turn_registry.c: per-session in-flight chat-turn registry (see turn_registry.h).
 *
 * The cancellation substrate for the server-owned turn lifecycle: a turn runs
 * to completion independent of its client connection and is stopped only by an
 * explicit cancel or its own bounds. */
#include "turn_registry.h"

#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define TURN_MAX PRESENCE_MAX

static turn_entry_t g_turns[TURN_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;

void turn_registry_init(void)
{
   pthread_mutex_lock(&g_lock);
   if (!g_inited)
   {
      memset(g_turns, 0, sizeof(g_turns));
      g_inited = 1;
   }
   pthread_mutex_unlock(&g_lock);
}

/* Caller must hold g_lock. */
static turn_entry_t *find_locked(const char *session_id)
{
   if (!session_id || !session_id[0])
      return NULL;
   for (int i = 0; i < TURN_MAX; i++)
      if (g_turns[i].in_use && strcmp(g_turns[i].session_id, session_id) == 0)
         return &g_turns[i];
   return NULL;
}

turn_entry_t *turn_registry_publish(const char *session_id, const char *turn_id)
{
   if (!session_id || !session_id[0])
      return NULL;
   pthread_mutex_lock(&g_lock);
   if (find_locked(session_id))
   {
      /* Collision: a turn is already in flight for this session. Under the
       * presence turn-lock this is impossible; treat as a hard error so we never
       * silently overwrite and orphan a child_pid. */
      pthread_mutex_unlock(&g_lock);
      LOG_WARN("turn_registry", "publish collision for session %s", session_id);
      return NULL;
   }
   turn_entry_t *slot = NULL;
   for (int i = 0; i < TURN_MAX; i++)
      if (!g_turns[i].in_use)
      {
         slot = &g_turns[i];
         break;
      }
   if (!slot)
   {
      pthread_mutex_unlock(&g_lock);
      LOG_WARN("turn_registry", "table full (%d); cannot register session %s", TURN_MAX,
               session_id);
      return NULL;
   }
   memset(slot, 0, sizeof(*slot));
   snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
   if (turn_id)
      snprintf(slot->turn_id, sizeof(slot->turn_id), "%s", turn_id);
   atomic_store(&slot->cancel, 0);
   slot->child_pid = -1;
   slot->reaped = 0;
   slot->in_use = 1;
   pthread_mutex_unlock(&g_lock);
   return slot;
}

void turn_registry_set_child(turn_entry_t *e, pid_t pid)
{
   if (!e)
      return;
   pthread_mutex_lock(&g_lock);
   if (e->in_use)
      e->child_pid = pid;
   pthread_mutex_unlock(&g_lock);
}

int turn_registry_cancel(const char *session_id, const char *owner_principal)
{
   /* Authz (cross-principal protection) is decided OUTSIDE the registry lock —
    * presence has its own lock and the leaf-mutex rule forbids holding g_lock
    * across a presence call. */
   if (owner_principal && owner_principal[0])
   {
      char owner[PRESENCE_OWNER_MAX];
      if (!presence_session_owner(session_id, owner, sizeof(owner)) ||
          strcmp(owner, owner_principal) != 0)
      {
         LOG_WARN("turn_registry", "refused cross-principal cancel of session %s", session_id);
         return -1;
      }
   }
   pthread_mutex_lock(&g_lock);
   turn_entry_t *e = find_locked(session_id);
   if (!e)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   atomic_store(&e->cancel, 1);
   pthread_mutex_unlock(&g_lock);
   return 1;
}

turn_entry_t *turn_registry_find(const char *session_id)
{
   pthread_mutex_lock(&g_lock);
   turn_entry_t *e = find_locked(session_id);
   pthread_mutex_unlock(&g_lock);
   return e;
}

void turn_registry_mark_reaped(turn_entry_t *e)
{
   if (!e)
      return;
   pthread_mutex_lock(&g_lock);
   e->reaped = 1;
   pthread_mutex_unlock(&g_lock);
}

void turn_registry_clear(turn_entry_t *e)
{
   if (!e)
      return;
   pthread_mutex_lock(&g_lock);
   memset(e, 0, sizeof(*e));
   pthread_mutex_unlock(&g_lock);
}

int turn_registry_cancel_all(void)
{
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < TURN_MAX; i++)
      if (g_turns[i].in_use)
      {
         atomic_store(&g_turns[i].cancel, 1);
         n++;
      }
   pthread_mutex_unlock(&g_lock);
   return n;
}

int turn_registry_sweep_dead(void)
{
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < TURN_MAX; i++)
   {
      turn_entry_t *e = &g_turns[i];
      if (!e->in_use)
         continue;
      /* owner thread gone? pthread_kill(.,0) returns ESRCH for a dead thread. */
      if (e->owner != 0 && pthread_kill(e->owner, 0) == ESRCH)
      {
         if (!e->reaped && e->child_pid > 0)
         {
            kill(e->child_pid, SIGKILL);
            waitpid(e->child_pid, NULL, 0);
         }
         LOG_WARN("turn_registry", "swept dead turn for session %s (owner gone)", e->session_id);
         memset(e, 0, sizeof(*e));
         n++;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return n;
}

/* turn_registry.c: per-session in-flight chat-turn registry (see turn_registry.h).
 *
 * The cancellation substrate for the server-owned turn lifecycle: a turn runs
 * to completion independent of its client connection and is stopped only by an
 * explicit cancel or its own bounds. */
#include "turn_registry.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --- pending-steer store -------------------------------------------------
 * One pending steering message per session: chat.interrupt stashes it and
 * cancels the in-flight turn; the worker that ran the cancelled turn takes it
 * and dispatches it as the next (server-initiated) turn. Same leaf lock as the
 * turn table; the message is a malloc'd copy owned by the slot until taken. */
typedef struct
{
   char session_id[PRESENCE_SESSION_ID_MAX];
   char *message;
   int in_use;
} steer_slot_t;

static steer_slot_t g_steers[TURN_MAX];

static steer_slot_t *steer_find_locked(const char *session_id)
{
   if (!session_id || !session_id[0])
      return NULL;
   for (int i = 0; i < TURN_MAX; i++)
      if (g_steers[i].in_use && strcmp(g_steers[i].session_id, session_id) == 0)
         return &g_steers[i];
   return NULL;
}

int chat_steer_set(const char *session_id, const char *message)
{
   if (!session_id || !session_id[0] || !message)
      return -1;
   char *copy = strdup(message);
   if (!copy)
      return -1;
   pthread_mutex_lock(&g_lock);
   steer_slot_t *slot = steer_find_locked(session_id);
   if (!slot)
      for (int i = 0; i < TURN_MAX; i++)
         if (!g_steers[i].in_use)
         {
            slot = &g_steers[i];
            slot->in_use = 1;
            snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
            slot->message = NULL;
            break;
         }
   if (slot)
   {
      free(slot->message); /* a newer steer supersedes an untaken one */
      slot->message = copy;
      copy = NULL;
   }
   pthread_mutex_unlock(&g_lock);
   free(copy);           /* non-NULL only when the table was full */
   return slot ? 0 : -1; /* -1 => caller must report interrupted=false */
}

int chat_steer_take(const char *session_id, char **out_message)
{
   if (out_message)
      *out_message = NULL;
   if (!session_id || !session_id[0] || !out_message)
      return 0;
   pthread_mutex_lock(&g_lock);
   steer_slot_t *slot = steer_find_locked(session_id);
   int got = 0;
   if (slot)
   {
      *out_message = slot->message; /* transfer ownership to caller */
      memset(slot, 0, sizeof(*slot));
      got = *out_message ? 1 : 0;
   }
   pthread_mutex_unlock(&g_lock);
   return got;
}

void chat_steer_clear(const char *session_id)
{
   char *msg = NULL;
   (void)chat_steer_take(session_id, &msg);
   free(msg);
}

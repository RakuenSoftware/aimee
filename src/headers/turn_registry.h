/* turn_registry.h: per-session in-flight chat-turn registry for
 * connection-independent cancellation (server-owned turn lifecycle, Phase 1).
 *
 * A chat turn no longer dies with its client connection. Once started it runs
 * to completion regardless of who is watching; the only ways to stop it are an
 * explicit cancel (session close, server shutdown, chat.graceful_cancel) or its
 * own token/deadline bounds. This registry is the cancellation substrate: one
 * entry per in-flight turn, holding an atomic cancel flag the worker polls and
 * the child pid the worker (the single reaper) signals.
 *
 * Invariant: at most one in-flight turn per session (enforced upstream by the
 * presence turn-lock), so entries key on session id. `turn_id` is carried for
 * observability and stream matching.
 *
 * Lock discipline: the registry mutex is a LEAF. It is never held while
 * acquiring a presence or compute lock, and no caller may hold a presence or
 * compute lock across a registry call. A cancel sets the atomic flag and reads
 * the pid under the mutex, then releases it BEFORE any kill()/waitpid(), which
 * only the owning worker performs.
 *
 * Thread-safety: every function is safe to call concurrently. */
#ifndef DEC_TURN_REGISTRY_H
#define DEC_TURN_REGISTRY_H 1

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>

#include "presence.h" /* PRESENCE_SESSION_ID_MAX / _TURN_ID_MAX / PRESENCE_MAX */

#ifdef __cplusplus
extern "C"
{
#endif

   /* One in-flight turn. `cancel` is the only field read on the worker hot path
    * (atomic, lock-free). `child_pid`/`reaped` are owned by the worker; the
    * worker is the sole caller of kill()/waitpid() for its child. */
   typedef struct turn_entry
   {
      char session_id[PRESENCE_SESSION_ID_MAX];
      char turn_id[PRESENCE_TURN_ID_MAX];
      atomic_int cancel;
      pid_t child_pid;
      int reaped;
      int in_use;
   } turn_entry_t;

   /* Initialize the global registry. Idempotent; call at server startup. */
   void turn_registry_init(void);

   /* Publish an entry for (session_id, turn_id) before the worker is dispatched,
    * so a cancel arriving immediately is never lost. Returns the entry (caller
    * caches the pointer for lock-free cancel reads) or NULL on collision (the
    * session already has an in-flight entry — a bug under the turn-lock
    * invariant) or when the table is full. On NULL the caller runs the turn
    * without a registry entry (still bounded by token/deadline limits). */
   turn_entry_t *turn_registry_publish(const char *session_id, const char *turn_id);

   /* Record the worker's child subprocess pid (CLI path, after fork). */
   void turn_registry_set_child(turn_entry_t *e, pid_t pid);

   /* Request cancellation of session_id's in-flight turn. `owner_principal`, when
    * non-NULL/non-empty, must match the session's presence owner or the cancel
    * is refused (cross-principal protection); pass NULL to bypass the check for
    * trusted internal callers (session close, shutdown). Returns 1 if a turn was
    * flagged, 0 if no such turn, -1 if refused on authz. */
   int turn_registry_cancel(const char *session_id, const char *owner_principal);

   /* Look up the entry for a session (NULL if none). */
   turn_entry_t *turn_registry_find(const char *session_id);

   /* True (non-zero) if this entry has been asked to cancel. Lock-free atomic
    * read for the worker hot path. NULL-safe (returns 0). */
   static inline int turn_entry_cancelled(turn_entry_t *e)
   {
      return e ? atomic_load(&e->cancel) : 0;
   }

   /* Mark the entry's child reaped (single writer: the owning worker). */
   void turn_registry_mark_reaped(turn_entry_t *e);

   /* Clear/free the entry after the worker has reaped its child and emitted
    * turn_done. No-op if e is NULL. */
   void turn_registry_clear(turn_entry_t *e);

   /* Flag every in-flight turn for cancellation (server shutdown). Returns the
    * number flagged. Sets flags only; the owning workers do the reaping. */
   int turn_registry_cancel_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_TURN_REGISTRY_H */

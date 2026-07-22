/* agent_shell.h: abstract driver interface for interactive agent CLI sessions */
#ifndef DEC_AGENT_SHELL_H
#define DEC_AGENT_SHELL_H 1

#include <signal.h>

/* Normalized event types emitted by all drivers */
typedef enum
{
   SHELL_EVENT_TEXT_DELTA,    /* streaming text token: data = token string */
   SHELL_EVENT_TOOL_START,    /* tool call beginning: data = tool name */
   SHELL_EVENT_TOOL_COMPLETE, /* tool done: data = NULL */
   SHELL_EVENT_SESSION_ID,    /* session ID update: data = new session_id */
   SHELL_EVENT_TURN_COMPLETE, /* response finished: data = NULL */
   SHELL_EVENT_ERROR,         /* error: data = message */
} agent_shell_event_t;

typedef void (*agent_shell_cb_t)(agent_shell_event_t event, const char *data, void *user);

typedef struct agent_shell_driver
{
   const char *name; /* "claude", "codex", "gemini" */

   /* Open a session. resume_id may be NULL for a new session.
    * Returns a heap-allocated opaque handle, or NULL on failure.
    * The handle must be freed via close(). */
   void *(*open)(const struct agent_shell_driver *d, const char *resume_id);

   /* Send a user turn into the open session.
    * Returns 0 on success, -1 on error. */
   int (*send)(void *handle, const char *message);

   /* Read and dispatch events until the turn completes or errors.
    * Calls cb for each event. interrupted is checked each iteration.
    * Returns 0 on clean completion, -1 on error. */
   int (*recv)(void *handle, agent_shell_cb_t cb, void *user, volatile int *interrupted);

   /* Close and free the handle. */
   void (*close)(void *handle);
} agent_shell_driver_t;

/* --- Driver registry --- */

/* Register a driver. Must be called before agent_shell_driver_get(). */
void agent_shell_driver_register(const agent_shell_driver_t *d);

/* Look up a driver by name. Returns NULL if not found. */
const agent_shell_driver_t *agent_shell_driver_get(const char *name);

#endif /* DEC_AGENT_SHELL_H */

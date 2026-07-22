/* agent_shell.c: registry for agent shell drivers (claude, codex, gemini). */
#include "agent_shell.h"
#include <pthread.h>
#include <string.h>

#define MAX_SHELL_DRIVERS 16

static const agent_shell_driver_t *g_drivers[MAX_SHELL_DRIVERS];
static int g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Forward declarations for built-in drivers */
extern agent_shell_driver_t claude_shell_driver;
extern agent_shell_driver_t claude_pty_shell_driver;
extern agent_shell_driver_t codex_shell_driver;
extern agent_shell_driver_t gemini_shell_driver;

static void register_builtins(void)
{
   agent_shell_driver_register(&claude_shell_driver);
   agent_shell_driver_register(&claude_pty_shell_driver);
   agent_shell_driver_register(&codex_shell_driver);
   agent_shell_driver_register(&gemini_shell_driver);
}

void agent_shell_driver_register(const agent_shell_driver_t *d)
{
   if (!d || !d->name)
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_drivers[i]->name, d->name) == 0)
      {
         pthread_mutex_unlock(&g_lock);
         return;
      }
   }
   if (g_count < MAX_SHELL_DRIVERS)
      g_drivers[g_count++] = d;
   pthread_mutex_unlock(&g_lock);
}

const agent_shell_driver_t *agent_shell_driver_get(const char *name)
{
   if (!name || !name[0])
      return NULL;
   pthread_once(&g_once, register_builtins);
   pthread_mutex_lock(&g_lock);
   const agent_shell_driver_t *found = NULL;
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_drivers[i]->name, name) == 0)
      {
         found = g_drivers[i];
         break;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

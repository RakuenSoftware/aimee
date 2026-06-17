/* test_webuser_editor.c — WP-I: the per-webuser code-server supervisor's gating,
 * identity validation, and bookkeeping. The actual code-server spawn is
 * deploy-validated (CI has no code-server binary), so here we exercise the
 * fail-closed paths: feature off, binary absent, non-webuser principal, and the
 * idle/touch/stop/shutdown bookkeeping on an empty registry. */
#include "webuser_editor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
   int port = -1;
   char err[256];

   /* Feature OFF by default (AIMEE_WEBCHAT_EDITOR unset). */
   unsetenv("AIMEE_WEBCHAT_EDITOR");
   assert(webuser_editor_available() == 0);
   assert(webuser_editor_ensure("webuser:alice", &port, err, sizeof(err)) == 0);

   /* Enabled but no code-server binary → still unavailable (fail closed). The
    * override points at a path that cannot be executed. */
   setenv("AIMEE_WEBCHAT_EDITOR", "1", 1);
   setenv("AIMEE_WEBCHAT_EDITOR_BIN", "/nonexistent/code-server", 1);
   assert(webuser_editor_available() == 0);
   /* (binary path is cached after the first resolve, so ensure() also returns 0) */
   assert(webuser_editor_ensure("webuser:alice", &port, err, sizeof(err)) == 0);

   /* Identity guard: only webuser: principals may drive an editor. NULL/non
    * webuser → -1, never a spawn. */
   assert(webuser_editor_ensure("uid:1000", &port, err, sizeof(err)) == -1);
   assert(webuser_editor_ensure(NULL, &port, err, sizeof(err)) == -1);
   assert(webuser_editor_ensure("webuser:bob", NULL, err, sizeof(err)) == -1);

   /* Bookkeeping ops are safe no-ops with no editors running. */
   webuser_editor_touch("webuser:alice");
   webuser_editor_touch(NULL);
   webuser_editor_stop("webuser:alice");
   webuser_editor_stop(NULL);
   assert(webuser_editor_reap_idle(0) == 0);  /* idle_secs<=0 reaps nothing */
   assert(webuser_editor_reap_idle(60) == 0); /* empty registry */
   webuser_editor_shutdown();

   printf("webuser_editor: ok\n");
   return 0;
}

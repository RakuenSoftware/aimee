#include "hook_session_token.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform_test_util.h"

int main(void)
{
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-hook-token-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   hook_session_token_registry_reset();

   char first[HOOK_SESSION_TOKEN_CAP], second[HOOK_SESSION_TOKEN_CAP];
   time_t expires = 0;
   assert(hook_session_token_mint("session-1", "claude", "uid:1000", first, &expires) == 0);
   assert(strlen(first) == HOOK_SESSION_TOKEN_HEX_LEN);
   assert(expires > time(NULL));
   assert(hook_session_token_verify("session-1", "claude", "uid:1000", first) == 1);
   assert(hook_session_token_verify("session-2", "claude", "uid:1000", first) == 0);
   assert(hook_session_token_verify("session-1", "codex", "uid:1000", first) == 0);
   assert(hook_session_token_verify("session-1", "claude", "uid:1001", first) == 0);
   assert(hook_session_token_verify("session-1", "claude", "uid:1000", "bad") == 0);

   assert(hook_session_token_store(home, "session-1", "claude", first) == 0);
   char loaded[HOOK_SESSION_TOKEN_CAP];
   assert(hook_session_token_load(home, "session-1", "claude", loaded) == 0);
   assert(strcmp(loaded, first) == 0);
   char path[4096];
   snprintf(path, sizeof(path), "%s/hook-tokens/session-1.claude", home);
   struct stat st;
   assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

   /* Resume rotates the token for exactly the same binding. */
   assert(hook_session_token_mint("session-1", "claude", "uid:1000", second, NULL) == 0);
   assert(strcmp(first, second) != 0);
   assert(hook_session_token_verify("session-1", "claude", "uid:1000", first) == 0);
   assert(hook_session_token_verify("session-1", "claude", "uid:1000", second) == 1);
   hook_session_token_revoke("session-1", "claude", "uid:1000");
   assert(hook_session_token_verify("session-1", "claude", "uid:1000", second) == 0);

   assert(hook_session_token_delete(home, "session-1", "claude") == 0);
   assert(hook_session_token_load(home, "session-1", "claude", loaded) == -1);
   assert(hook_session_token_store(home, "../escape", "claude", second) == -1);

   char dir[4096];
   snprintf(dir, sizeof(dir), "%s/hook-tokens", home);
   assert(rmdir(dir) == 0);
   assert(rmdir(home) == 0);
   puts("hook_session_token: all tests passed");
   return 0;
}

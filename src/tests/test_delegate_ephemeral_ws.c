/* test_delegate_ephemeral_ws.c: server-side ephemeral workspace helper —
 * deleg_id validation (traversal/separators), create (dir + note), remove
 * (cleanup + refuses paths outside <home>/delegate-ws). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "delegate_ephemeral_ws.h"
#include "platform_path.h"
#include "platform_test_util.h"

static int path_exists(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0;
}

int main(void)
{
   printf("test_delegate_ephemeral_ws:\n");

   char home[512];
   snprintf(home, sizeof(home), "%s/aimee_ews_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);

   char out[1024];

   /* 1. Unsafe deleg_ids are rejected and leave out empty (fail closed). */
   assert(delegate_ephemeral_ws_create(NULL, out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("../escape", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("a/b", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("a/../b", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("bad;rm -rf", out, sizeof(out)) == -1);
   assert(out[0] == '\0');
   printf("  rejects_unsafe_ids: ok\n");

   /* 2. A valid id creates the dir under <home>/delegate-ws and drops a note. */
   assert(delegate_ephemeral_ws_create("deleg-1-2-3", out, sizeof(out)) == 0);
   char expect[700];
   snprintf(expect, sizeof(expect), "%s/delegate-ws/deleg-1-2-3", home);
   assert(strcmp(out, expect) == 0);
   assert(path_exists(out));
   char note[1100];
   snprintf(note, sizeof(note), "%s/AIMEE_WORKSPACE_NOTE.txt", out);
   assert(path_exists(note));
   printf("  creates_dir_and_note: ok\n");

   /* 3. remove() cleans up the workspace it created. */
   delegate_ephemeral_ws_remove(out);
   assert(!path_exists(out));
   printf("  remove_cleans_up: ok\n");

   /* 4. remove() refuses paths not under <home>/delegate-ws (safety). */
   char keep[700];
   snprintf(keep, sizeof(keep), "%s/keepme", home);
   assert(platform_mkdir_p(keep, 0700) == 0);
   delegate_ephemeral_ws_remove(keep); /* under home but not delegate-ws -> no-op */
   delegate_ephemeral_ws_remove("/tmp"); /* outside home -> no-op */
   assert(path_exists(keep));
   printf("  remove_refuses_outside_prefix: ok\n");

   platform_test_rmrf(home);
   printf("All delegate_ephemeral_ws tests passed.\n");
   return 0;
}

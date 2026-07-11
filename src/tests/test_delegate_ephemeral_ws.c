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
   /* dotted specials: "." / ".." collapse onto delegate-ws itself; leading-'.'
    * hidden names are rejected too. */
   assert(delegate_ephemeral_ws_create(".", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create("..", out, sizeof(out)) == -1);
   assert(delegate_ephemeral_ws_create(".x", out, sizeof(out)) == -1);
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

   /* 5. remove() must NOT follow a symlink under delegate-ws to delete outside it. */
   char victim_dir[700];
   snprintf(victim_dir, sizeof(victim_dir), "%s/victim", home);
   assert(platform_mkdir_p(victim_dir, 0700) == 0);
   char victim_file[820];
   snprintf(victim_file, sizeof(victim_file), "%s/precious.txt", victim_dir);
   FILE *vf = fopen(victim_file, "w");
   assert(vf != NULL);
   fputs("do not delete\n", vf);
   fclose(vf);

   char wsroot[700];
   snprintf(wsroot, sizeof(wsroot), "%s/delegate-ws", home);
   assert(platform_mkdir_p(wsroot, 0700) == 0);
   char evil_link[820];
   snprintf(evil_link, sizeof(evil_link), "%s/evil", wsroot);
   assert(symlink(victim_dir, evil_link) == 0); /* delegate-ws/evil -> ../victim */

   delegate_ephemeral_ws_remove(evil_link); /* lexical prefix OK, but it's a symlink */
   assert(path_exists(victim_dir));  /* target dir untouched */
   assert(path_exists(victim_file)); /* target contents untouched */
   printf("  remove_refuses_symlink_escape: ok\n");

   platform_test_rmrf(home);
   printf("All delegate_ephemeral_ws tests passed.\n");
   return 0;
}

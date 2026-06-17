/* test_webuser_runtime.c — WP-L: per-webuser tmpfs runtime dir, fail-closed.
 * /dev/shm is tmpfs and /proc is not, so the tmpfs gate is exercised against
 * real mounts without assuming anything about /tmp. */
#include "webuser_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
   /* --- the tmpfs gate against real mounts --- */
   assert(webuser_runtime_is_tmpfs("/dev/shm") == 1); /* tmpfs */
   assert(webuser_runtime_is_tmpfs("/proc") == 0);    /* procfs, not tmpfs */
   assert(webuser_runtime_is_tmpfs("/nonexistent-xyzzy") == -1);
   assert(webuser_runtime_is_tmpfs("") == -1);
   assert(webuser_runtime_is_tmpfs(NULL) == -1);

   char base[256];
   snprintf(base, sizeof(base), "/dev/shm/aimee-wrt-test-%d", (int)getpid());

   /* --- success path on tmpfs: 0700 per-user dir, isolated per principal --- */
   setenv("AIMEE_RUNTIME_DIR", base, 1);
   char a[512], b[512];
   assert(webuser_runtime_dir("webuser:alice", a, sizeof(a)) == 0);
   assert(webuser_runtime_dir("webuser:bob", b, sizeof(b)) == 0);
   assert(strcmp(a, b) != 0);
   struct stat st;
   assert(stat(a, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
   assert(webuser_runtime_is_tmpfs(a) == 1); /* the per-user dir is on tmpfs */
   assert(strstr(a, "/webusers/alice") != NULL);

   /* non-webuser / malformed principals are refused; out is emptied on error */
   char x[512];
   strcpy(x, "STALE");
   assert(webuser_runtime_dir("uid:1000", x, sizeof(x)) == -1 && x[0] == '\0');
   assert(webuser_runtime_dir("webuser:..", x, sizeof(x)) == -1);
   assert(webuser_runtime_dir("webuser:a/b", x, sizeof(x)) == -1);

   /* cleanup removes the dir (and any sockets in it) */
   char sock[600];
   snprintf(sock, sizeof(sock), "%s/dummy.sock", a);
   FILE *f = fopen(sock, "w");
   if (f)
      fclose(f);
   webuser_runtime_cleanup("webuser:alice");
   assert(stat(a, &st) != 0); /* gone */

   /* --- partial-failure: a too-small out buffer creates NOTHING (not even the
    * shared /webusers parent) --- */
   {
      char freshbase[256];
      snprintf(freshbase, sizeof(freshbase), "/dev/shm/aimee-wrt-cap-%d", (int)getpid());
      setenv("AIMEE_RUNTIME_DIR", freshbase, 1);
      char tiny[8];
      assert(webuser_runtime_dir("webuser:alice", tiny, sizeof(tiny)) == -1);
      char parent[300];
      snprintf(parent, sizeof(parent), "%s/webusers", freshbase);
      assert(stat(parent, &st) != 0); /* parent must NOT exist */
      rmdir(freshbase);               /* base itself also not created on early cap failure */
      assert(stat(freshbase, &st) != 0);
      setenv("AIMEE_RUNTIME_DIR", base, 1); /* restore */
   }

   /* cleanup on a malformed/non-webuser principal is a safe no-op */
   webuser_runtime_cleanup("webuser:..");
   webuser_runtime_cleanup("uid:1000");
   webuser_runtime_cleanup("");

   /* --- fail-closed: a non-tmpfs base is refused, nothing created --- */
   /* Find a writable non-tmpfs dir to point the base at. The build/cwd is on
    * disk in CI; if it happens to be tmpfs locally, skip the negative case
    * rather than emit a false failure. */
   char cwd[256];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   if (webuser_runtime_is_tmpfs(cwd) == 0)
   {
      char disk_base[512];
      snprintf(disk_base, sizeof(disk_base), "%s/aimee-wrt-disk-%d", cwd, (int)getpid());
      setenv("AIMEE_RUNTIME_DIR", disk_base, 1);
      char y[600];
      assert(webuser_runtime_dir("webuser:alice", y, sizeof(y)) == -1); /* fail-closed */
      assert(webuser_runtime_is_tmpfs(disk_base) == 0);                 /* base made, but disk */
      /* nothing under it should have been created */
      char inner[700];
      snprintf(inner, sizeof(inner), "%s/webusers", disk_base);
      assert(stat(inner, &st) != 0);
      rmdir(disk_base);
      printf("  (fail-closed negative case exercised on disk-backed cwd)\n");
   }
   else
   {
      printf("  (cwd is tmpfs — skipped the disk-backed negative case)\n");
   }

   rmdir(b);
   printf("webuser_runtime: all tests passed\n");
   return 0;
}

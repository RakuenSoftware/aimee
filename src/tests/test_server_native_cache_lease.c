#include "server_native_cache_lease.h"
#include "platform_test_util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static const char binding[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static char home[4096], directory[4096];
static server_native_cache_lease_t *open_lease(const char *turn, char error[512])
{
   server_native_cache_lease_t *lease = server_native_cache_lease_acquire_unpublished(
       home, "codex", "session", binding, "actor", turn, NULL, error);
   if (lease)
      assert(!server_native_cache_lease_update(lease, server_native_cache_lease_manifest(lease)));
   return lease;
}
static void path(char out[8192], const char *name)
{
   assert(snprintf(out, 8192, "%s/%s", directory, name) < 8192);
}
static void wait_exit(pid_t child)
{
   int status;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void sdk_identity(void)
{
   const char other_binding[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
   char error[512], first[65], repeated[65], other[65], file[8192], extra[8192], parent[8192],
       moved[8192];
   server_native_cache_lease_t *lease = server_native_cache_lease_acquire_unpublished(
       home, "claude", "identity-session", binding, "actor", "A1", NULL, error);
   assert(lease && !server_native_cache_sdk_identity(lease, first));
   assert(strlen(first) == 64 && strspn(first, "0123456789abcdef") == 64);
   assert(!server_native_cache_sdk_identity(lease, repeated) && !strcmp(first, repeated));
   assert(!server_native_cache_lease_close(lease, 1));
   lease = server_native_cache_lease_acquire_unpublished(home, "claude", "identity-session",
                                                         binding, "actor", "A2", NULL, error);
   assert(lease && !server_native_cache_sdk_identity(lease, repeated) && !strcmp(first, repeated));
   assert(snprintf(parent, sizeof parent, "%s/browser-claude/identity-session/%s", home, binding) <
          (int)sizeof parent);
   assert(snprintf(file, sizeof file, "%s/.aimee-claude-sdk-user-id", parent) < (int)sizeof file);
   struct stat status;
   assert(!lstat(file, &status) && S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600 &&
          status.st_nlink == 1);
   assert(!chmod(file, 0644) && server_native_cache_sdk_identity(lease, repeated) < 0 &&
          !repeated[0]);
   assert(!chmod(file, 0600));
   assert(snprintf(extra, sizeof extra, "%s-extra", file) < (int)sizeof extra);
   assert(!link(file, extra) && server_native_cache_sdk_identity(lease, repeated) < 0);
   assert(!unlink(extra));
   assert(!rename(file, extra) && !symlink(extra, file));
   assert(server_native_cache_sdk_identity(lease, repeated) < 0);
   assert(!unlink(file) && !rename(extra, file));
   FILE *f = fopen(file, "w");
   assert(f && fwrite("bad", 1, 3, f) == 3 && !fclose(f));
   assert(server_native_cache_sdk_identity(lease, repeated) < 0);
   assert(!stat(file, &status) && status.st_size == 3);
   f = fopen(file, "w");
   assert(f && fwrite(first, 1, 64, f) == 64 && fputc('\n', f) == '\n' && !fclose(f));
   assert(server_native_cache_sdk_identity(lease, repeated) < 0);
   f = fopen(file, "w");
   assert(f && fwrite(first, 1, 64, f) == 64 && !fclose(f));
   assert(!server_native_cache_sdk_identity(lease, repeated) && !strcmp(first, repeated));
   assert(snprintf(moved, sizeof moved, "%s-moved", parent) < (int)sizeof moved);
   assert(!rename(parent, moved) && !mkdir(parent, 0700));
   assert(server_native_cache_sdk_identity(lease, repeated) < 0);
   assert(!rmdir(parent) && !rename(moved, parent));
   assert(!server_native_cache_lease_close(lease, 1));
   lease = server_native_cache_lease_acquire_unpublished(home, "claude", "identity-session",
                                                         other_binding, "actor", "B", NULL, error);
   assert(lease && !server_native_cache_sdk_identity(lease, other) && strcmp(first, other));
   assert(!server_native_cache_lease_close(lease, 1));
   const char *bindings[] = {binding, other_binding};
   for (unsigned i = 0; i < 2; i++)
   {
      assert(snprintf(parent, sizeof parent, "%s/browser-claude/identity-session/%s", home,
                      bindings[i]) < (int)sizeof parent);
      assert(snprintf(file, sizeof file, "%s/.aimee-claude-sdk-user-id", parent) <
                 (int)sizeof file &&
             !unlink(file));
      assert(snprintf(file, sizeof file, "%s/.aimee-native-owner.lock", parent) <
                 (int)sizeof file &&
             !unlink(file));
      assert(!rmdir(parent));
   }
   *strrchr(parent, '/') = 0;
   assert(!rmdir(parent));
   *strrchr(parent, '/') = 0;
   assert(!rmdir(parent));
}
static void private_workspace(void)
{
   const char other_binding[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
   const char epoch[] = "11111111111111111111111111111111";
   char error[512], a[8192], b[8192], fresh[8192], saved[8192];
   server_native_cache_lease_t *lease = server_native_cache_lease_acquire_unpublished(
       home, "claude", "workspace-session", binding, "actor", "A1", NULL, error);
   assert(lease);
   int first = server_native_cache_workspace_open(lease, "0", a, sizeof a);
   assert(first >= 0 && server_native_cache_workspace_valid(lease, "0", first, a));
   struct stat original, returned;
   assert(!fstat(first, &original));
   close(first);
   assert(!server_native_cache_lease_close(lease, 1));
   lease = server_native_cache_lease_acquire_unpublished(home, "claude", "workspace-session",
                                                         other_binding, "actor", "B", NULL, error);
   assert(lease);
   int other = server_native_cache_workspace_open(lease, "0", b, sizeof b);
   assert(other >= 0 && strcmp(a, b));
   close(other);
   assert(!server_native_cache_lease_close(lease, 1));
   lease = server_native_cache_lease_acquire_unpublished(home, "claude", "workspace-session",
                                                         binding, "actor", "A2", NULL, error);
   assert(lease);
   first = server_native_cache_workspace_open(lease, "0", b, sizeof b);
   assert(first >= 0 && !strcmp(a, b) && !fstat(first, &returned) &&
          original.st_dev == returned.st_dev && original.st_ino == returned.st_ino);
   other = server_native_cache_workspace_open(lease, epoch, fresh, sizeof fresh);
   assert(other >= 0 && strcmp(a, fresh));
   assert(!server_native_cache_workspace_valid(lease, epoch, first, a));
   assert(server_native_cache_workspace_open(lease, "../0", b, sizeof b) < 0);
   assert(server_native_cache_workspace_open(lease, "", b, sizeof b) < 0);
   assert(server_native_cache_workspace_open(lease, "../escape", b, sizeof b) < 0);
   close(other);
   assert(!chmod(a, 0770));
   assert(!server_native_cache_workspace_valid(lease, "0", first, a));
   assert(server_native_cache_workspace_open(lease, "0", b, sizeof b) < 0);
   assert(!chmod(a, 0600));
   assert(!server_native_cache_workspace_valid(lease, "0", first, a));
   assert(!chmod(a, 0700));
   assert(snprintf(saved, sizeof saved, "%s-moved", a) < (int)sizeof saved);
   assert(!rename(a, saved) && !mkdir(a, 0700));
   assert(!server_native_cache_workspace_valid(lease, "0", first, a));
   assert(!rmdir(a) && !symlink(saved, a));
   assert(!server_native_cache_workspace_valid(lease, "0", first, a));
   assert(server_native_cache_workspace_open(lease, "0", b, sizeof b) < 0);
   assert(!unlink(a) && !rename(saved, a));
   assert(server_native_cache_workspace_valid(lease, "0", first, a));
   /* Parent replacement is refused even if the retained leaf is still valid. */
   char parent[8192], parent_saved[8192];
   snprintf(parent, sizeof parent, "%s", a);
   *strrchr(parent, '/') = 0;
   assert(snprintf(parent_saved, sizeof parent_saved, "%s-moved", parent) <
          (int)sizeof parent_saved);
   assert(!rename(parent, parent_saved) && !symlink(parent_saved, parent));
   assert(!server_native_cache_workspace_valid(lease, "0", first, a));
   assert(server_native_cache_workspace_open(lease, "0", b, sizeof b) < 0);
   assert(!unlink(parent) && !rename(parent_saved, parent));
   assert(server_native_cache_workspace_valid(lease, "0", first, a));
   close(first);
   assert(!server_native_cache_lease_close(lease, 1));
   assert(!rmdir(a) && !rmdir(fresh));
   const char *bindings[] = {binding, other_binding};
   for (unsigned i = 0; i < 2; i++)
   {
      assert(snprintf(a, sizeof a, "%s/browser-claude/workspace-session/%s/sdk-workspaces%s", home,
                      bindings[i], i ? "/0" : "") < (int)sizeof a);
      assert(!rmdir(a));
      if (i)
      {
         *strrchr(a, '/') = 0;
         assert(!rmdir(a));
      }
      assert(snprintf(a, sizeof a,
                      "%s/browser-claude/workspace-session/%s/.aimee-native-owner.lock", home,
                      bindings[i]) < (int)sizeof a);
      assert(!unlink(a));
      *strrchr(a, '/') = 0;
      assert(!rmdir(a));
   }
   assert(snprintf(a, sizeof a, "%s/browser-claude/workspace-session", home) < (int)sizeof a);
   assert(!rmdir(a));
   *strrchr(a, '/') = 0;
   assert(!rmdir(a));
}
int main(void)
{
   assert(snprintf(home, sizeof home, "%s/native-cache-lease-XXXXXX", platform_tmpdir()) <
          (int)sizeof home);
   assert(mkdtemp(home));
   sdk_identity();
   private_workspace();
   assert(snprintf(directory, sizeof directory, "%s/browser-codex/session/%s", home, binding) <
          (int)sizeof directory);
   char error[512], marker[8192], lock[8192], cache[8192], replacement[8192];
   path(marker, ".aimee-native-owner.json");
   path(lock, ".aimee-native-owner.lock");
   path(cache, "committed-conversation");
   path(replacement, "replacement");
   server_native_cache_lease_t *lease = open_lease("turn-one", error);
   assert(lease);
   assert(!open_lease("turn-two", error) && strstr(error, "busy"));
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      assert(!open_lease("parallel-context", error) && strstr(error, "busy"));
      _exit(0);
   }
   wait_exit(child);
   assert(!server_native_cache_lease_close(lease, 1));
   assert(access(marker, F_OK) && !access(lock, F_OK));
   cJSON *unpublished_prior = NULL;
   lease = server_native_cache_lease_acquire_unpublished(home, "codex", "session", binding, "actor",
                                                         "before-db", &unpublished_prior, error);
   assert(lease && !unpublished_prior && access(marker, F_OK));
   assert(server_native_cache_lease_owner(lease));
   assert(!server_native_cache_lease_close(lease, 0));
   // A crash before DB registration publishes no claim marker or SDK effects.
   lease = server_native_cache_lease_acquire_unpublished(home, "codex", "session", binding, "actor",
                                                         "after-db", &unpublished_prior, error);
   assert(lease && !unpublished_prior && access(marker, F_OK));
   cJSON *registered = cJSON_Duplicate(server_native_cache_lease_manifest(lease), 1);
   assert(cJSON_AddStringToObject(registered, "epoch", "0"));
   assert(!server_native_cache_lease_update(lease, registered));
   assert(!access(marker, F_OK));
   cJSON_Delete(registered);
   assert(!server_native_cache_lease_close(lease, 1));
   lease = open_lease("durable-update", error);
   assert(lease);
   char first_owner[33];
   snprintf(first_owner, sizeof first_owner, "%s", server_native_cache_lease_owner(lease));
   cJSON *updated = cJSON_Duplicate(server_native_cache_lease_manifest(lease), 1);
   assert(cJSON_AddStringToObject(updated, "containment_json", "{\"exact\":\"saved-boundary\"}"));
   assert(!server_native_cache_lease_update(lease, updated));
   assert(!server_native_cache_lease_close(lease, 0));
   cJSON *prior = NULL;
   lease = server_native_cache_lease_acquire_unpublished(home, "codex", "session", binding, "actor",
                                                         "next", &prior, error);
   assert(lease && prior && cJSON_Compare(updated, prior, 1));
   assert(!strcmp(first_owner, server_native_cache_lease_owner(lease)));
   cJSON_ReplaceItemInObjectCaseSensitive(updated, "actor", cJSON_CreateString("foreign"));
   assert(server_native_cache_lease_update(lease, updated));
   cJSON_Delete(updated);
   cJSON_Delete(prior);
   assert(!server_native_cache_lease_rotate_unpublished(lease, "after-reconcile", error));
   assert(!server_native_cache_lease_update(lease, server_native_cache_lease_manifest(lease)));
   assert(strcmp(first_owner, server_native_cache_lease_owner(lease)));
   assert(!cJSON_GetObjectItemCaseSensitive(server_native_cache_lease_manifest(lease),
                                            "containment_json"));
   assert(!server_native_cache_lease_rotate_unpublished(lease, "before-new-reservation", error));
   assert(access(marker, F_OK));
   // A crash between reconciled-marker retirement and fresh DB40 leaves no
   // unauthoritative new marker; the physical lock remains held until close.
   assert(!open_lease("parallel-after-retirement", error) && strstr(error, "busy"));
   assert(!server_native_cache_lease_close(lease, 0));
   unpublished_prior = NULL;
   lease = server_native_cache_lease_acquire_unpublished(home, "codex", "session", binding, "actor",
                                                         "restart-after-retirement",
                                                         &unpublished_prior, error);
   assert(lease && !unpublished_prior && access(marker, F_OK));
   assert(!server_native_cache_lease_close(lease, 1));
   int file = open(cache, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(file >= 0 && write(file, "COMMITTED\n", 10) == 10);
   close(file);

#ifdef __linux__
   // Real owner death releases flock, while an escaped setsid descendant keeps
   // writing SDK cache data. Restart must still refuse the durable marker.
   assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
   int ready[2];
   assert(!pipe(ready));
   child = fork();
   assert(child >= 0);
   if (!child)
   {
      close(ready[0]);
      assert(open_lease("crashed-turn", error));
      pid_t writer = fork();
      assert(writer >= 0);
      if (!writer)
      {
         assert(setsid() >= 0);
         for (int fd = 3; fd < 1024; fd++)
            if (fd != ready[1])
               close(fd); // Like descriptor-native exec: no inherited lease.
         int output = open(cache, O_WRONLY | O_APPEND);
         assert(output >= 0);
         char byte = 'x';
         assert(write(output, &byte, 1) == 1);
         pid_t self = getpid();
         assert(write(ready[1], &self, sizeof self) == sizeof self);
         close(ready[1]);
         for (;;)
         {
            assert(write(output, &byte, 1) == 1);
            usleep(10000);
         }
      }
      close(ready[1]);
      for (;;)
         pause();
   }
   close(ready[1]);
   pid_t writer;
   assert(read(ready[0], &writer, sizeof writer) == sizeof writer);
   close(ready[0]);
   assert(!kill(child, SIGKILL));
   int status;
   assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
   assert(!open_lease("restart", error) && strstr(error, "unresolved prior owner"));
   struct stat before, after;
   assert(!stat(cache, &before));
   usleep(60000);
   assert(!stat(cache, &after) && after.st_size > before.st_size);
   assert(!kill(writer, SIGKILL));
   assert(waitpid(writer, &status, 0) == writer && WIFSIGNALED(status));
   // Even a positively dead test descendant alone is not a production recovery
   // contract. The marker remains refused; only fixture teardown removes it.
   assert(!open_lease("after-writer-exit", error) && strstr(error, "unresolved prior owner"));
   assert(!unlink(marker));
#endif

   lease = open_lease("replacement-turn", error);
   assert(lease);
   file = open(replacement, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(file >= 0 && write(file, "foreign", 7) == 7);
   close(file);
   assert(!rename(replacement, marker));
   assert(server_native_cache_lease_close(lease, 1) == -1);
   assert(!open_lease("foreign-marker", error) && strstr(error, "unresolved prior owner"));
   assert(!unlink(marker));
   assert(!unlink(lock));
   assert(!symlink(cache, lock));
   assert(!open_lease("unsafe-lock", error));
   assert(!unlink(lock));
   assert(!unlink(cache));
   assert(!rmdir(directory));
   char parent[8192];
   assert(snprintf(parent, sizeof parent, "%s/browser-codex/session", home) < (int)sizeof parent);
   assert(!rmdir(parent));
   assert(snprintf(parent, sizeof parent, "%s/browser-codex", home) < (int)sizeof parent);
   assert(!rmdir(parent));
   assert(!rmdir(home));
   puts("native cache pre-spawn lease, crash marker and escaped-writer refusal passed");
   return 0;
}

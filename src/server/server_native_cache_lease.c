#include "server_native_cache_lease.h"
#if defined(_WIN32)
#include <stddef.h>
#include <stdio.h>
server_native_cache_lease_t *server_native_cache_lease_open(const char *home, const char *sdk,
                                                            const char *session,
                                                            const char *binding, const char *actor,
                                                            const char *turn, char error[512])
{
   (void)home;
   (void)sdk;
   (void)session;
   (void)binding;
   (void)actor;
   (void)turn;
   if (error)
      snprintf(error, 512, "native cache directory custody is unavailable on this platform");
   return NULL;
}
int server_native_cache_lease_close(server_native_cache_lease_t *lease, int clean)
{
   (void)clean;
   return lease ? -1 : 0;
}
#else
#include "platform_random.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct server_native_cache_lease
{
   int directory, lock;
   struct stat marker;
};
static const char lock_name[] = ".aimee-native-owner.lock";
static const char marker_name[] = ".aimee-native-owner.json";

static int owned_directory(int parent, const char *name)
{
   int created = mkdirat(parent, name, 0700);
   if ((created && errno != EEXIST) || (!created && fsync(parent)))
      return -1;
   int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   struct stat status;
   if (fd >= 0 && (fstat(fd, &status) || status.st_uid != geteuid() || (status.st_mode & 0022)))
   {
      close(fd);
      return -1;
   }
   return fd;
}
static int label(const char *value)
{
   return value && value[0] && strlen(value) < 128 &&
          strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
              strlen(value);
}
int server_native_cache_lease_close(server_native_cache_lease_t *lease, int clean)
{
   if (!lease)
      return 0;
   int rc = 0;
   if (clean)
   {
      struct stat current;
      if (fstatat(lease->directory, marker_name, &current, AT_SYMLINK_NOFOLLOW) ||
          !S_ISREG(current.st_mode) || current.st_dev != lease->marker.st_dev ||
          current.st_ino != lease->marker.st_ino || current.st_nlink != 1 ||
          unlinkat(lease->directory, marker_name, 0) || fsync(lease->directory))
         rc = -1;
   }
   close(lease->lock);
   close(lease->directory);
   free(lease);
   return rc;
}
server_native_cache_lease_t *server_native_cache_lease_open(const char *home, const char *sdk,
                                                            const char *session,
                                                            const char *binding, const char *actor,
                                                            const char *turn, char error[512])
{
   if (error)
      error[0] = 0;
   if (!home || home[0] != '/' || !sdk || (strcmp(sdk, "codex") && strcmp(sdk, "claude")) ||
       !label(session) || !binding || strlen(binding) != 64 ||
       strspn(binding, "0123456789abcdef") != 64 || !actor || !actor[0] || strlen(actor) > 128 ||
       !turn || !turn[0] || strlen(turn) > 128)
      return NULL;
   int root = open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   char vendor[32];
   snprintf(vendor, sizeof vendor, "browser-%s", sdk);
   const char *parts[] = {vendor, session, binding};
   for (unsigned i = 0; root >= 0 && i < sizeof parts / sizeof parts[0]; i++)
   {
      int next = owned_directory(root, parts[i]);
      close(root);
      root = next;
   }
   if (root < 0)
      goto unavailable;
   int lock = openat(root, lock_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
   struct stat status, linked;
   if (lock < 0 || fstat(lock, &status) || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
       (status.st_mode & 0077) || status.st_nlink != 1 || flock(lock, LOCK_EX | LOCK_NB) ||
       fstatat(root, lock_name, &linked, AT_SYMLINK_NOFOLLOW) || linked.st_dev != status.st_dev ||
       linked.st_ino != status.st_ino)
   {
      if (error)
         snprintf(error, 512, "native cache directory is busy or its ownership lease is unsafe");
      if (lock >= 0)
         close(lock);
      close(root);
      return NULL;
   }
   // Presence is deliberately fail-closed, including corrupt or foreign markers.
   // Neither a missing PID nor an unlocked file proves descendant quiescence.
   int marker =
       openat(root, marker_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (marker < 0)
   {
      if (error)
         snprintf(error, 512,
                  "native cache has an unresolved prior owner; child-tree quiescence is required "
                  "before recovery");
      close(lock);
      close(root);
      return NULL;
   }
   server_native_cache_lease_t *lease = calloc(1, sizeof *lease);
   char nonce[33];
   cJSON *manifest = cJSON_CreateObject();
   int valid = lease && manifest && !platform_random_hex(nonce, 32) &&
               cJSON_AddNumberToObject(manifest, "version", 1) &&
               cJSON_AddStringToObject(manifest, "sdk", sdk) &&
               cJSON_AddStringToObject(manifest, "session_id", session) &&
               cJSON_AddStringToObject(manifest, "binding", binding) &&
               cJSON_AddStringToObject(manifest, "actor", actor) &&
               cJSON_AddStringToObject(manifest, "turn_id", turn) &&
               cJSON_AddStringToObject(manifest, "owner", nonce);
   char *wire = valid ? cJSON_PrintUnformatted(manifest) : NULL;
   cJSON_Delete(manifest);
   valid = wire && strlen(wire) <= 2048 &&
           write(marker, wire, strlen(wire)) == (ssize_t)strlen(wire) && !fsync(marker) &&
           !fstat(marker, &status) && !fsync(root);
   free(wire);
   close(marker);
   if (!valid)
   {
      // An interrupted durable publication remains unresolved, never an absent owner.
      free(lease);
      close(lock);
      close(root);
      goto unavailable;
   }
   lease->directory = root;
   lease->lock = lock;
   lease->marker = status;
   return lease;
unavailable:
   if (error)
      snprintf(error, 512, "could not retain native cache directory ownership");
   return NULL;
}

#endif

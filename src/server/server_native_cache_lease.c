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
server_native_cache_lease_t *server_native_cache_lease_acquire(const char *home, const char *sdk,
                                                               const char *session,
                                                               const char *binding,
                                                               const char *actor, const char *turn,
                                                               cJSON **prior, char error[512])
{
   if (prior)
      *prior = NULL;
   return server_native_cache_lease_open(home, sdk, session, binding, actor, turn, error);
}
server_native_cache_lease_t *server_native_cache_lease_acquire_unpublished(
    const char *home, const char *sdk, const char *session, const char *binding, const char *actor,
    const char *turn, cJSON **prior, char error[512])
{
   return server_native_cache_lease_acquire(home, sdk, session, binding, actor, turn, prior, error);
}
const cJSON *server_native_cache_lease_manifest(const server_native_cache_lease_t *lease)
{
   (void)lease;
   return NULL;
}
const char *server_native_cache_lease_owner(const server_native_cache_lease_t *lease)
{
   (void)lease;
   return NULL;
}
int server_native_cache_lease_update(server_native_cache_lease_t *lease, const cJSON *manifest)
{
   (void)lease;
   (void)manifest;
   return -1;
}
int server_native_cache_lease_rotate(server_native_cache_lease_t *lease, const char *turn,
                                     char error[512])
{
   (void)lease;
   (void)turn;
   (void)error;
   return -1;
}
int server_native_cache_lease_rotate_unpublished(server_native_cache_lease_t *lease,
                                                 const char *turn, char error[512])
{
   (void)lease;
   (void)turn;
   (void)error;
   return -1;
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
   cJSON *manifest;
   int published;
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
   if (clean && lease->published)
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
   cJSON_Delete(lease->manifest);
   free(lease);
   return rc;
}
static server_native_cache_lease_t *lease_acquire(const char *home, const char *sdk,
                                                  const char *session, const char *binding,
                                                  const char *actor, const char *turn,
                                                  cJSON **prior, char error[512], int publish_fresh)
{
   if (prior)
      *prior = NULL;
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
   struct stat presence;
   int absent = fstatat(root, marker_name, &presence, AT_SYMLINK_NOFOLLOW) && errno == ENOENT;
   int marker = publish_fresh ? openat(root, marker_name,
                                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)
                              : -1;
   if (!publish_fresh && !absent)
      errno = EEXIST;
   if (marker < 0 && (publish_fresh || !absent))
   {
      int saved = errno;
      int fd = saved == EEXIST ? openat(root, marker_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
      char wire[16385];
      struct stat existing, linked_marker;
      ssize_t count = fd >= 0 ? read(fd, wire, sizeof wire) : -1;
      cJSON *manifest = NULL;
      if (fd >= 0 && count > 0 && count < (ssize_t)sizeof wire && !fstat(fd, &existing) &&
          S_ISREG(existing.st_mode) && existing.st_uid == geteuid() && !(existing.st_mode & 0077) &&
          existing.st_nlink == 1 && existing.st_size == count &&
          !fstatat(root, marker_name, &linked_marker, AT_SYMLINK_NOFOLLOW) &&
          existing.st_dev == linked_marker.st_dev && existing.st_ino == linked_marker.st_ino)
      {
         wire[count] = 0;
         const char *end = NULL;
         manifest = cJSON_ParseWithOpts(wire, &end, 1);
      }
      if (fd >= 0)
         close(fd);
      const char *keys[] = {"sdk", "session_id", "binding", "actor", "turn_id", "owner"};
      const char *expected[] = {sdk, session, binding, actor};
      int valid = prior && cJSON_IsObject(manifest);
      for (unsigned i = 0; valid && i < 6; i++)
      {
         const cJSON *item = cJSON_GetObjectItemCaseSensitive(manifest, keys[i]);
         valid = cJSON_IsString(item) && item->valuestring[0] &&
                 (i < 4    ? !strcmp(item->valuestring, expected[i])
                  : i == 4 ? strlen(item->valuestring) <= 128
                           : strlen(item->valuestring) == 32 &&
                                 strspn(item->valuestring, "0123456789abcdef") == 32);
      }
      const cJSON *version = cJSON_GetObjectItemCaseSensitive(manifest, "version");
      valid = valid && cJSON_IsNumber(version) && version->valuedouble == 1;
      const cJSON *a, *b;
      cJSON_ArrayForEach(a, manifest) for (b = a->next; b;
                                           b = b->next) if (!a->string || !b->string ||
                                                            !strcmp(a->string, b->string)) valid =
          0;
      server_native_cache_lease_t *lease = valid ? calloc(1, sizeof *lease) : NULL;
      cJSON *copy = lease ? cJSON_Duplicate(manifest, 1) : NULL;
      if (lease && copy)
      {
         lease->directory = root;
         lease->lock = lock;
         lease->marker = existing;
         lease->manifest = manifest;
         lease->published = 1;
         *prior = copy;
         return lease;
      }
      cJSON_Delete(copy);
      cJSON_Delete(manifest);
      free(lease);
      if (error)
         snprintf(error, 512, "native cache has an unresolved prior owner or unsafe marker");
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
   valid = wire && strlen(wire) <= 2048 &&
           (!publish_fresh || (write(marker, wire, strlen(wire)) == (ssize_t)strlen(wire) &&
                               !fsync(marker) && !fstat(marker, &status) && !fsync(root)));
   free(wire);
   if (marker >= 0)
      close(marker);
   if (!valid)
   {
      // An interrupted durable publication remains unresolved, never an absent owner.
      cJSON_Delete(manifest);
      free(lease);
      close(lock);
      close(root);
      goto unavailable;
   }
   lease->directory = root;
   lease->lock = lock;
   if (publish_fresh)
      lease->marker = status;
   lease->published = publish_fresh;
   lease->manifest = manifest;
   return lease;
unavailable:
   if (error)
      snprintf(error, 512, "could not retain native cache directory ownership");
   return NULL;
}

const cJSON *server_native_cache_lease_manifest(const server_native_cache_lease_t *lease)
{
   return lease ? lease->manifest : NULL;
}
const char *server_native_cache_lease_owner(const server_native_cache_lease_t *lease)
{
   return lease ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lease->manifest, "owner"))
                : NULL;
}
static int replace_marker(server_native_cache_lease_t *lease, const cJSON *manifest)
{
   if (!lease || !cJSON_IsObject(manifest))
      return -1;
   char *wire = cJSON_PrintUnformatted(manifest);
   cJSON *copy = wire ? cJSON_Duplicate(manifest, 1) : NULL;
   char nonce[33], temporary[80];
   int rc = -1, fd = -1;
   struct stat current, next;
   if (!wire || !copy || strlen(wire) > 16384 || platform_random_hex(nonce, 32))
      goto done;
   snprintf(temporary, sizeof temporary, ".aimee-native-owner-%s.tmp", nonce);
   fd = openat(lease->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               0600);
   if (fd < 0)
      goto done;
   if (write(fd, wire, strlen(wire)) != (ssize_t)strlen(wire) || fsync(fd) || fstat(fd, &next))
   {
      unlinkat(lease->directory, temporary, 0);
      goto done;
   }
   if (lease->published)
   {
      if (fstatat(lease->directory, marker_name, &current, AT_SYMLINK_NOFOLLOW) ||
          current.st_dev != lease->marker.st_dev || current.st_ino != lease->marker.st_ino ||
          !S_ISREG(current.st_mode) || current.st_nlink != 1 ||
          renameat(lease->directory, temporary, lease->directory, marker_name))
      {
         unlinkat(lease->directory, temporary, 0);
         goto done;
      }
   }
   else
   {
      // Link refuses replacement if any marker appeared after our initial read.
      if (linkat(lease->directory, temporary, lease->directory, marker_name, 0))
      {
         unlinkat(lease->directory, temporary, 0);
         goto done;
      }
      if (unlinkat(lease->directory, temporary, 0))
         goto done;
   }
   lease->published = 1;
   lease->marker = next;
   cJSON_Delete(lease->manifest);
   lease->manifest = copy;
   copy = NULL;
   rc = fsync(lease->directory) ? -1 : 0;
done:
   if (fd >= 0)
      close(fd);
   cJSON_Delete(copy);
   free(wire);
   return rc;
}
int server_native_cache_lease_update(server_native_cache_lease_t *lease, const cJSON *manifest)
{
   if (!lease || !cJSON_IsObject(manifest))
      return -1;
   const char *keys[] = {"version", "sdk", "session_id", "binding", "actor", "turn_id", "owner"};
   for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
      if (!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(lease->manifest, keys[i]),
                         cJSON_GetObjectItemCaseSensitive(manifest, keys[i]), 1))
         return -1;
   const cJSON *a, *b;
   cJSON_ArrayForEach(a, manifest) for (b = a->next; b;
                                        b = b->next) if (!a->string || !b->string ||
                                                         !strcmp(a->string, b->string)) return -1;
   return replace_marker(lease, manifest);
}
int server_native_cache_lease_rotate_unpublished(server_native_cache_lease_t *lease,
                                                 const char *turn, char error[512])
{
   if (error)
      error[0] = 0;
   if (!lease || !turn || !turn[0] || strlen(turn) > 128)
      return -1;
   char nonce[33];
   cJSON *manifest = cJSON_CreateObject();
   const char *keys[] = {"version", "sdk", "session_id", "binding", "actor"};
   int valid = manifest && !platform_random_hex(nonce, 32);
   for (unsigned i = 0; valid && i < sizeof keys / sizeof keys[0]; i++)
   {
      cJSON *item = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(lease->manifest, keys[i]), 1);
      valid = item && cJSON_AddItemToObject(manifest, keys[i], item);
   }
   valid = valid && cJSON_AddStringToObject(manifest, "turn_id", turn) &&
           cJSON_AddStringToObject(manifest, "owner", nonce);
   struct stat current;
   if (valid && lease->published)
   {
      valid = !fstatat(lease->directory, marker_name, &current, AT_SYMLINK_NOFOLLOW) &&
              S_ISREG(current.st_mode) && current.st_nlink == 1 &&
              current.st_dev == lease->marker.st_dev && current.st_ino == lease->marker.st_ino &&
              !unlinkat(lease->directory, marker_name, 0);
      if (valid)
      {
         lease->published = 0;
         valid = !fsync(lease->directory);
      }
   }
   if (!valid)
   {
      cJSON_Delete(manifest);
      if (error)
         snprintf(error, 512, "could not retire exact reconciled native cache marker");
      return -1;
   }
   cJSON_Delete(lease->manifest);
   lease->manifest = manifest;
   return 0;
}
int server_native_cache_lease_rotate(server_native_cache_lease_t *lease, const char *turn,
                                     char error[512])
{
   if (error)
      error[0] = 0;
   if (!lease || !turn || !turn[0] || strlen(turn) > 128)
      return -1;
   char nonce[33];
   cJSON *manifest = cJSON_CreateObject();
   const char *keys[] = {"version", "sdk", "session_id", "binding", "actor"};
   int valid = manifest && !platform_random_hex(nonce, 32);
   for (unsigned i = 0; valid && i < sizeof keys / sizeof keys[0]; i++)
   {
      cJSON *item = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(lease->manifest, keys[i]), 1);
      valid = item && cJSON_AddItemToObject(manifest, keys[i], item);
   }
   valid = valid && cJSON_AddStringToObject(manifest, "turn_id", turn) &&
           cJSON_AddStringToObject(manifest, "owner", nonce);
   int rc = valid ? replace_marker(lease, manifest) : -1;
   cJSON_Delete(manifest);
   if (rc && error)
      snprintf(error, 512, "could not durably rotate exact native cache ownership");
   return rc;
}
server_native_cache_lease_t *server_native_cache_lease_acquire(const char *home, const char *sdk,
                                                               const char *session,
                                                               const char *binding,
                                                               const char *actor, const char *turn,
                                                               cJSON **prior, char error[512])
{
   return lease_acquire(home, sdk, session, binding, actor, turn, prior, error, 1);
}
server_native_cache_lease_t *server_native_cache_lease_acquire_unpublished(
    const char *home, const char *sdk, const char *session, const char *binding, const char *actor,
    const char *turn, cJSON **prior, char error[512])
{
   return lease_acquire(home, sdk, session, binding, actor, turn, prior, error, 0);
}
server_native_cache_lease_t *server_native_cache_lease_open(const char *home, const char *sdk,
                                                            const char *session,
                                                            const char *binding, const char *actor,
                                                            const char *turn, char error[512])
{
   return server_native_cache_lease_acquire(home, sdk, session, binding, actor, turn, NULL, error);
}

#endif

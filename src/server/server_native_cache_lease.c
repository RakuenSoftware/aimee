#include "server_native_cache_lease.h"
#include "platform_random.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct server_native_cache_lease
{
   int directory, lock;
   char home[PATH_MAX];
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
static int workspace_epoch(const char *epoch)
{
   return epoch &&
          (!strcmp(epoch, "0") || (strlen(epoch) == 32 && strspn(epoch, "0123456789abcdef") == 32));
}
static int directory_safe(int fd, int private)
{
   struct stat st;
   return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
          (private ? (st.st_mode & 0777) == 0700 : !(st.st_mode & 0022));
}
static int directory_same(int left, int right)
{
   struct stat a, b;
   return !fstat(left, &a) && !fstat(right, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
/* Rewalk the host-owned cache namespace rather than trusting an absolute leaf
 * path with unchecked intermediate components. Detect renamed lease roots too. */
static int workspace_root(const server_native_cache_lease_t *lease, char *path, size_t capacity)
{
   if (!lease || !lease->manifest || !path || !capacity)
      return -1;
   const char *sdk = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lease->manifest, "sdk"));
   const char *session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lease->manifest, "session_id"));
   const char *binding =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lease->manifest, "binding"));
   char vendor[32];
   if (!sdk || !session || !binding ||
       snprintf(vendor, sizeof vendor, "browser-%s", sdk) >= (int)sizeof vendor ||
       snprintf(path, capacity, "%s/%s/%s/%s", lease->home, vendor, session, binding) >=
           (int)capacity)
      return -1;
   int root = open(lease->home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   const char *parts[] = {vendor, session, binding};
   for (unsigned i = 0; root >= 0 && i < sizeof parts / sizeof parts[0]; i++)
   {
      int next = directory_safe(root, 0)
                     ? openat(root, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                     : -1;
      close(root);
      root = next;
   }
   if (!directory_safe(root, 0) || !directory_same(root, lease->directory))
   {
      if (root >= 0)
         close(root);
      return -1;
   }
   return root;
}
static int sdk_identity_read(int root, char identity[65])
{
   const char *name = ".aimee-claude-sdk-user-id";
   int fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
   if (fd < 0)
      return errno == ENOENT ? 1 : -1;
   struct stat st, linked;
   char bytes[65];
   size_t used = 0;
   int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
               (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size == 64;
   while (valid && used < sizeof bytes)
   {
      ssize_t n = read(fd, bytes + used, sizeof bytes - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n < 0)
         valid = 0;
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   valid = valid && used == 64 && !fstatat(root, name, &linked, AT_SYMLINK_NOFOLLOW) &&
           linked.st_dev == st.st_dev && linked.st_ino == st.st_ino;
   close(fd);
   if (!valid)
      return -1;
   bytes[64] = 0;
   if (strspn(bytes, "0123456789abcdef") != 64)
      return -1;
   memcpy(identity, bytes, 65);
   return 0;
}
int server_native_cache_sdk_identity(server_native_cache_lease_t *lease, char identity[65])
{
   if (!identity)
      return -1;
   identity[0] = 0;
   const char *sdk =
       lease && lease->manifest
           ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lease->manifest, "sdk"))
           : NULL;
   if (!sdk || strcmp(sdk, "claude"))
      return -1;
   char root_path[PATH_MAX];
   int root = workspace_root(lease, root_path, sizeof root_path);
   if (root < 0)
      return -1;
   int rc = sdk_identity_read(root, identity);
   if (rc != 1)
   {
      close(root);
      return rc;
   }
   char generated[65], nonce[33], temporary[96];
   if (platform_random_hex(generated, 64) || platform_random_hex(nonce, 32))
   {
      close(root);
      return -1;
   }
   snprintf(temporary, sizeof temporary, ".aimee-claude-sdk-user-id-%s.tmp", nonce);
   int fd = openat(root, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   int created = fd >= 0, valid = created;
   size_t sent = 0;
   while (valid && sent < 64)
   {
      ssize_t n = write(fd, generated + sent, 64 - sent);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         valid = 0;
      else
         sent += (size_t)n;
   }
   if (valid)
      valid = !fsync(fd);
   if (fd >= 0 && close(fd))
      valid = 0;
   /* Never replace an existing identity, even after malformed state or a race. */
   if (valid)
      valid = !linkat(root, temporary, root, ".aimee-claude-sdk-user-id", 0);
   if (created && unlinkat(root, temporary, 0))
      valid = 0;
   if (valid)
      valid = !fsync(root) && !sdk_identity_read(root, identity);
   close(root);
   if (!valid)
      identity[0] = 0;
   return valid ? 0 : -1;
}
int server_native_cache_workspace_open(server_native_cache_lease_t *lease, const char *epoch,
                                       char *path, size_t capacity)
{
   if (!path || !capacity)
      return -1;
   path[0] = 0;
   if (!workspace_epoch(epoch))
      return -1;
   char root_path[PATH_MAX];
   int root = workspace_root(lease, root_path, sizeof root_path);
   if (root < 0)
      return -1;
   int parent = owned_directory(root, "sdk-workspaces");
   close(root);
   int fd = directory_safe(parent, 1) ? owned_directory(parent, epoch) : -1;
   if (parent >= 0)
      close(parent);
   if (!directory_safe(fd, 1) ||
       snprintf(path, capacity, "%s/sdk-workspaces/%s", root_path, epoch) >= (int)capacity ||
       !server_native_cache_workspace_valid(lease, epoch, fd, path))
   {
      if (fd >= 0)
         close(fd);
      path[0] = 0;
      return -1;
   }
   return fd;
}
int server_native_cache_workspace_valid(const server_native_cache_lease_t *lease, const char *epoch,
                                        int fd, const char *path)
{
   if (!workspace_epoch(epoch) || !directory_safe(fd, 1) || !path)
      return 0;
   char root_path[PATH_MAX], expected[PATH_MAX];
   int root = workspace_root(lease, root_path, sizeof root_path);
   if (root < 0)
      return 0;
   int parent = openat(root, "sdk-workspaces", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   close(root);
   int current = directory_safe(parent, 1)
                     ? openat(parent, epoch, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                     : -1;
   if (parent >= 0)
      close(parent);
   int valid = directory_safe(current, 1) && directory_same(current, fd) &&
               snprintf(expected, sizeof expected, "%s/sdk-workspaces/%s", root_path, epoch) <
                   (int)sizeof expected &&
               !strcmp(path, expected);
   if (current >= 0)
      close(current);
   return valid;
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
                                                  cJSON **prior, char error[512])
{
   if (prior)
      *prior = NULL;
   if (error)
      error[0] = 0;
   if (!home || home[0] != '/' || strlen(home) >= PATH_MAX || !sdk ||
       (strcmp(sdk, "codex") && strcmp(sdk, "claude")) || !label(session) || !binding ||
       strlen(binding) != 64 || strspn(binding, "0123456789abcdef") != 64 || !actor || !actor[0] ||
       strlen(actor) > 128 || !turn || !turn[0] || strlen(turn) > 128)
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
   if (!absent)
   {
      int fd = openat(root, marker_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
         snprintf(lease->home, sizeof lease->home, "%s", home);
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
   if (!valid)
   {
      cJSON_Delete(manifest);
      free(lease);
      close(lock);
      close(root);
      goto unavailable;
   }
   snprintf(lease->home, sizeof lease->home, "%s", home);
   lease->directory = root;
   lease->lock = lock;
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
server_native_cache_lease_t *server_native_cache_lease_acquire_unpublished(
    const char *home, const char *sdk, const char *session, const char *binding, const char *actor,
    const char *turn, cJSON **prior, char error[512])
{
   return lease_acquire(home, sdk, session, binding, actor, turn, prior, error);
}

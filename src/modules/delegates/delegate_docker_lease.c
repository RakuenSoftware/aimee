#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
/* Durable creation custody, qualified daemon recovery, and exact retirement.
 * No execution/admission or image filesystem operation belongs to this owner. */
#include "delegate_docker_internal.h"
#include "aimee_home.h"
#include "cJSON.h"
#include "util.h"
#include "log.h"
#include "platform_random.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/file.h>
#include <dirent.h>
#endif
const char *resolve_docker_bin(void)
{
   const char *override = getenv("AIMEE_DOCKER_BIN");
   if (override && override[0])
      return override;
   return "docker";
}
const char *resolve_delegate_egress_bin(void)
{
   const char *override = getenv("AIMEE_DELEGATE_EGRESS_BIN");
   return override && override[0] ? override : "aimee-delegate-egress";
}
/* Checked commands use the same client selected by the sterile creator PATH,
 * never an executor's replacement PATH. Resolve symlinks before capturing it. */
int docker_capture_binary(docker_state_t *state)
{
   const char *configured = state->docker_binary;
   char resolved[MAX_PATH_LEN], candidate[MAX_PATH_LEN];
   if (strchr(configured, '/'))
   {
      if (!realpath(configured, resolved))
         return -1;
   }
   else
   {
      const char *directories[] = {"/usr/local/bin", "/usr/bin", "/bin", NULL};
      int found = 0;
      for (int i = 0; directories[i]; i++)
         if (snprintf(candidate, sizeof candidate, "%s/%s", directories[i], configured) <
                 (int)sizeof candidate &&
             !access(candidate, X_OK) && realpath(candidate, resolved))
         {
            found = 1;
            break;
         }
      if (!found)
         return -1;
   }
   struct stat status;
   if (stat(resolved, &status) || !S_ISREG(status.st_mode) || access(resolved, X_OK) ||
       snprintf(state->docker_binary, sizeof state->docker_binary, "%s", resolved) >=
           (int)sizeof state->docker_binary)
      return -1;
   return 0;
}

/* Own only the journal/nonce parent for ordinary resources. The global callback
 * socket is a verified mount fact, never this lease's removable artifact. */
int docker_lease_open(docker_state_t *state, const char *private_socket)
{
#if !defined(__linux__)
   (void)state;
   (void)private_socket;
   return -1;
#else
   char parent_path[MAX_PATH_LEN];
   int home = -1, parent = -1;
   struct stat status;
   if (private_socket)
   {
      const char *leaf = strrchr(private_socket, '/');
      if (!leaf || snprintf(parent_path, sizeof parent_path, "%.*s", (int)(leaf - private_socket),
                            private_socket) >= (int)sizeof parent_path)
         return -1;
      parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   }
   else
   {
      const char *root = aimee_home();
      if (!root || root[0] != '/' ||
          (home = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
         return -1;
      if ((mkdirat(home, "acp-owners", 0700) && errno != EEXIST) ||
          (state->lease_owners_fd =
               openat(home, "acp-owners", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
          fstat(state->lease_owners_fd, &status) || status.st_uid != geteuid() ||
          (status.st_mode & 0777) != 0700 || platform_random_hex(state->lease_leaf, 64) ||
          mkdirat(state->lease_owners_fd, state->lease_leaf, 0700) ||
          snprintf(parent_path, sizeof parent_path, "%s/acp-owners/%s", root, state->lease_leaf) >=
              (int)sizeof parent_path)
      {
         close(home);
         return -1;
      }
      close(home);
      parent = openat(state->lease_owners_fd, state->lease_leaf,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      state->retired = 1; /* No creation call exists until this returns and Go commits intent. */
   }
   int valid = parent >= 0 && !fstat(parent, &status) && status.st_uid == geteuid() &&
               (status.st_mode & 0777) == 0700 &&
               snprintf(state->lease_check, sizeof state->lease_check, "%s/lease.jsonl",
                        parent_path) < (int)sizeof state->lease_check;
   if (valid)
      state->lease_parent_fd = fcntl(parent, F_DUPFD_CLOEXEC, 10);
   if (valid)
      state->lease_fd =
          openat(parent, "lease.jsonl", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   valid = valid && state->lease_fd >= 0 && state->lease_parent_fd >= 0 &&
           !flock(state->lease_fd, LOCK_EX | LOCK_NB) &&
           !fstat(state->lease_fd, &state->lease_identity) &&
           !fstat(parent, &state->parent_identity) && !fsync(parent);
   if (valid && private_socket)
   {
      state->artifact_helper_fd = openat(parent, "owner-io", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      state->artifact_socket_fd =
          openat(parent, "aimee-http.sock", O_PATH | O_NOFOLLOW | O_CLOEXEC);
      valid = state->artifact_helper_fd >= 0 && state->artifact_socket_fd >= 0 &&
              !fstat(state->artifact_helper_fd, &state->helper_identity) &&
              S_ISREG(state->helper_identity.st_mode) &&
              !fstat(state->artifact_socket_fd, &state->socket_identity) &&
              S_ISSOCK(state->socket_identity.st_mode);
      const char *nonce = strrchr(parent_path, '/');
      char owners_path[MAX_PATH_LEN];
      if (valid)
         valid = nonce && strlen(nonce + 1) == 64 && strspn(nonce + 1, "0123456789abcdef") == 64 &&
                 snprintf(owners_path, sizeof owners_path, "%.*s", (int)(nonce - parent_path),
                          parent_path) < (int)sizeof owners_path &&
                 (state->lease_owners_fd =
                      open(owners_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) >= 0;
      struct stat named;
      if (valid)
         valid = !fstatat(state->lease_owners_fd, nonce + 1, &named, AT_SYMLINK_NOFOLLOW) &&
                 named.st_dev == state->parent_identity.st_dev &&
                 named.st_ino == state->parent_identity.st_ino;
      if (valid)
         snprintf(state->lease_leaf, sizeof state->lease_leaf, "%s", nonce + 1);
   }
   if (valid)
      valid = !fsync(state->lease_owners_fd);
   if (parent >= 0)
      close(parent);
   if (valid)
      state->retired = 0;
   return valid ? 0 : -1;
#endif
}

/* Capacity is reserved before strict creation. Failed cleanup transfers its
 * exact held lease here until the existing reaper confirms retirement. */
#define DOCKER_OWNED_CLEANUP_MAX 16
static pthread_mutex_t cleanup_lock = PTHREAD_MUTEX_INITIALIZER;
static docker_state_t *cleanup_slots[DOCKER_OWNED_CLEANUP_MAX];
int docker_cleanup_reserve(docker_state_t *state)
{
   pthread_mutex_lock(&cleanup_lock);
   for (int i = 0; i < DOCKER_OWNED_CLEANUP_MAX; i++)
      if (!cleanup_slots[i])
      {
         cleanup_slots[i] = state;
         state->cleanup_slot = i;
         pthread_mutex_unlock(&cleanup_lock);
         return 0;
      }
   pthread_mutex_unlock(&cleanup_lock);
   return -1;
}
static void docker_cleanup_release(docker_state_t *state)
{
   pthread_mutex_lock(&cleanup_lock);
   if (state->cleanup_slot >= 0 && state->cleanup_slot < DOCKER_OWNED_CLEANUP_MAX &&
       cleanup_slots[state->cleanup_slot] == state)
      cleanup_slots[state->cleanup_slot] = NULL;
   pthread_mutex_unlock(&cleanup_lock);
}
static int docker_source_identity(const cJSON *object, struct stat *identity)
{
   const char *dev = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "dev"));
   const char *inode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "inode"));
   if (!dev || !dev[0] || strspn(dev, "0123456789") != strlen(dev) || !inode || !inode[0] ||
       strspn(inode, "0123456789") != strlen(inode))
      return -1;
   errno = 0;
   unsigned long long device = strtoull(dev, NULL, 10);
   if (errno)
      return -1;
   unsigned long long number = strtoull(inode, NULL, 10);
   if (errno || !number || (unsigned long long)(dev_t)device != device ||
       (unsigned long long)(ino_t)number != number)
      return -1;
   identity->st_dev = (dev_t)device;
   identity->st_ino = (ino_t)number;
   return 0;
}
static int docker_cleanup_lease_mode(docker_state_t *state, int recover)
{
   if (!state || state->lease_fd < 0 || state->retired)
      return 0;
   if (state->cleanup_failed)
      return -1;
   const char *argv[] = {resolve_delegate_egress_bin(),
                         "cleanup-lease",
                         "--lease-fd",
                         "3",
                         "--lease-check",
                         state->lease_check,
                         "--engine-endpoint",
                         state->engine_endpoint,
                         "--docker",
                         state->docker_binary,
                         recover ? "--recover" : NULL,
                         NULL};
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent",
                          NULL};
   char *output = NULL;
   int rc = safe_exec_capture_cwd_env_fd_timeout(argv, NULL, environment, &output, 4096, 45000,
                                                 state->lease_fd, 3);
   cJSON *receipt = !rc && output ? cJSON_ParseWithOpts(output, NULL, 1) : NULL;
   const char *schema = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "schema"));
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "container_id"));
   int valid_schema = schema && !strcmp(schema, "aimee.container-cleanup.v1");
   int removed = valid_schema &&
                 cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "removed")) && id &&
                 strlen(id) == 64 && strspn(id, "0123456789abcdef") == 64 &&
                 !cJSON_GetObjectItemCaseSensitive(receipt, "no_container") &&
                 (!state->container_id[0] || !strcmp(id, state->container_id));
   int no_container = valid_schema && !state->container_id[0] &&
                      !cJSON_GetObjectItemCaseSensitive(receipt, "container_id") &&
                      cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(receipt, "removed")) &&
                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "no_container"));
   if (recover)
   {
      const cJSON *identities = cJSON_GetObjectItemCaseSensitive(receipt, "source_identities");
      const char *mode =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "resource_mode"));
      int ordinary = mode && !strcmp(mode, "ordinary-container");
      if (!removed || (mode && !ordinary) ||
          docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "parent"),
                                 &state->parent_identity) ||
          (ordinary && (cJSON_GetObjectItemCaseSensitive(identities, "socket") ||
                        cJSON_GetObjectItemCaseSensitive(identities, "helper"))) ||
          (!ordinary &&
           (docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "socket"),
                                   &state->socket_identity) ||
            docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "helper"),
                                   &state->helper_identity))))
         removed = no_container = 0;
      state->private_io = !ordinary;
   }
   cJSON_Delete(receipt);
   free(output);
   if (!removed && !no_container)
   {
      state->cleanup_failed = 1;
      return -1;
   }
   state->retired = 1;
   return 0;
}
int docker_cleanup_lease(docker_state_t *state)
{
   return docker_cleanup_lease_mode(state, 0);
}
static int docker_unlink_owned(int parent, const char *name, const struct stat *identity, int flags)
{
   struct stat current;
   if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW))
      return errno == ENOENT ? 0 : -1;
   if (current.st_dev != identity->st_dev || current.st_ino != identity->st_ino)
      return -1; /* A replaced path is never cleanup authority. */
   return unlinkat(parent, name, flags);
}
static void docker_release_artifacts(docker_state_t *state)
{
   if (state->lease_parent_fd < 0 || !state->retired)
      return;
   int failed =
       docker_unlink_owned(state->lease_parent_fd, "lease.jsonl", &state->lease_identity, 0);
   if (state->private_io)
   {
      failed |= docker_unlink_owned(state->lease_parent_fd, "owner-io", &state->helper_identity, 0);
      failed |= docker_unlink_owned(state->lease_parent_fd, "aimee-http.sock",
                                    &state->socket_identity, 0);
   }
   if (!failed && state->lease_owners_fd >= 0)
      failed = docker_unlink_owned(state->lease_owners_fd, state->lease_leaf,
                                   &state->parent_identity, AT_REMOVEDIR);
   if (failed)
      LOG_WARN("delegate-sandbox", "owned cleanup artifacts changed; unmatched paths retained");
   else if (state->lease_owners_fd >= 0)
      (void)fsync(state->lease_owners_fd);
}
void docker_state_destroy(docker_state_t *state)
{
   if (state)
   {
      cli_native_stdio_close(&state->file_owner);
      cli_native_stdio_close(&state->forwarder);
      cli_native_stdio_close(&state->package_forwarder);
      if (state->launcher_fd >= 0)
         close(state->launcher_fd);
      state->launcher_fd = -1;
      if (state->launcher_cwd_fd >= 0)
         close(state->launcher_cwd_fd);
      state->launcher_cwd_fd = -1;
   }
   if (state && docker_cleanup_lease(state))
   {
      state->file_validate = NULL;
      state->file_context = NULL;
      pthread_mutex_lock(&cleanup_lock);
      state->cleanup_pending = 1;
      pthread_mutex_unlock(&cleanup_lock);
      LOG_ERROR("delegate-sandbox",
                "strict lease cleanup unconfirmed; transferred to bounded cleanup custody");
      return;
   }
   if (state)
      docker_release_artifacts(state);
   if (state && state->lease_fd >= 0)
      close(state->lease_fd);
   if (state && state->artifact_helper_fd >= 0)
      close(state->artifact_helper_fd);
   if (state && state->artifact_socket_fd >= 0)
      close(state->artifact_socket_fd);
   if (state && state->lease_parent_fd >= 0)
      close(state->lease_parent_fd);
   if (state && state->lease_owners_fd >= 0)
      close(state->lease_owners_fd);
   if (state && state->engine_fd >= 0)
      close(state->engine_fd);
   if (state)
      docker_cleanup_release(state);
   free(state);
}
int docker_reap_pending(void)
{
   docker_state_t *state = NULL;
   pthread_mutex_lock(&cleanup_lock);
   for (int i = 0; i < DOCKER_OWNED_CLEANUP_MAX; i++)
      if (cleanup_slots[i] && cleanup_slots[i]->cleanup_pending == 1)
      {
         state = cleanup_slots[i];
         state->cleanup_pending = 2;
         state->cleanup_failed = 0;
         break;
      }
   pthread_mutex_unlock(&cleanup_lock);
   if (!state)
      return 0;
   if (!docker_cleanup_lease(state))
   {
      docker_state_destroy(state);
      return 1;
   }
   pthread_mutex_lock(&cleanup_lock);
   state->cleanup_pending = 1;
   pthread_mutex_unlock(&cleanup_lock);
   return 0;
}
int docker_pin_engine(docker_state_t *state)
{
#if !defined(__linux__)
   (void)state;
   return -1;
#else
   const char *configured = getenv("DOCKER_HOST");
   const char *path = configured && configured[0] ? configured : "unix:///var/run/docker.sock";
   if (strncmp(path, "unix:///", 8))
      return -1;
   int raw = open(path + 7, O_PATH | O_NOFOLLOW | O_CLOEXEC);
   state->engine_fd = raw >= 0 ? cli_native_pin_fd(raw) : -1;
   if (raw >= 0)
      close(raw);
   struct stat status;
   if (state->engine_fd < 0 || fstat(state->engine_fd, &status) || !S_ISSOCK(status.st_mode) ||
       snprintf(state->engine_endpoint, sizeof state->engine_endpoint, "unix:///proc/%ld/fd/%d",
                (long)getpid(), state->engine_fd) >= (int)sizeof state->engine_endpoint)
      return -1;
   return 0;
#endif
}

/* Restart cleanup shares the existing reaper, never the prefix sweep. The
 * only discovery root is this process's configured Aimee home. A live creator,
 * helper or pending owner keeps the journal OFD locked. Each pass advances a
 * bounded directory cursor and attempts at most one qualified journal. */
int docker_recover_journal(void)
{
#if !defined(__linux__)
   return 0;
#else
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   static long cursor;
   static dev_t device;
   static ino_t inode;
   if (pthread_mutex_trylock(&lock))
      return 0;
   int removed = 0, home = -1, owners = -1;
   DIR *directory = NULL;
   const char *configured_home = aimee_home();
   char root[MAX_PATH_LEN];
   struct stat status;
   if (!configured_home || configured_home[0] != '/' ||
       snprintf(root, sizeof root, "%s", configured_home) >= (int)sizeof root ||
       (home = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
       (owners = openat(home, "acp-owners", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
       fstat(owners, &status) || status.st_uid != geteuid() || (status.st_mode & 0777) != 0700)
      goto done;
   if (device != status.st_dev || inode != status.st_ino)
   {
      cursor = 0;
      device = status.st_dev;
      inode = status.st_ino;
   }
   directory = fdopendir(owners);
   if (!directory)
      goto done;
   owners = -1;
   if (cursor)
      seekdir(directory, cursor);
   for (int count = 0; count < 32; count++)
   {
      struct dirent *entry = readdir(directory);
      if (!entry)
      {
         cursor = 0;
         break;
      }
      cursor = telldir(directory);
      if (strlen(entry->d_name) != 64 || strspn(entry->d_name, "0123456789abcdef") != 64)
         continue;
      docker_state_t *state = calloc(1, sizeof *state);
      if (!state)
         break;
      state->engine_fd = state->lease_fd = state->lease_parent_fd = state->lease_owners_fd = -1;
      state->lease_parent_fd =
          openat(dirfd(directory), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      int valid = state->lease_parent_fd >= 0 && !fstat(state->lease_parent_fd, &status) &&
                  status.st_uid == geteuid() && (status.st_mode & 0777) == 0700;
      if (valid)
         state->lease_fd = openat(state->lease_parent_fd, "lease.jsonl",
                                  O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      valid = valid && state->lease_fd >= 0 && !fstat(state->lease_fd, &state->lease_identity) &&
              S_ISREG(state->lease_identity.st_mode) && state->lease_identity.st_uid == geteuid() &&
              (state->lease_identity.st_mode & 0777) == 0600 &&
              state->lease_identity.st_nlink == 1 && state->lease_identity.st_size > 0 &&
              state->lease_identity.st_size <= (1 << 20) + 4096 &&
              !flock(state->lease_fd, LOCK_EX | LOCK_NB);
      if (valid)
      {
         valid =
             snprintf(state->lease_check, sizeof state->lease_check, "%s/acp-owners/%s/lease.jsonl",
                      root, entry->d_name) < (int)sizeof state->lease_check &&
             snprintf(state->docker_binary, sizeof state->docker_binary, "%s",
                      resolve_docker_bin()) < (int)sizeof state->docker_binary;
         snprintf(state->lease_leaf, sizeof state->lease_leaf, "%s", entry->d_name);
         state->lease_owners_fd = fcntl(dirfd(directory), F_DUPFD_CLOEXEC, 10);
         if (valid && state->lease_owners_fd >= 0 && !docker_capture_binary(state) &&
             !docker_pin_engine(state) && !docker_cleanup_lease_mode(state, 1))
         {
            docker_release_artifacts(state);
            removed = 1;
         }
         else
            LOG_WARN("delegate-sandbox",
                     "restart cleanup unconfirmed for private scope %s; journal retained",
                     entry->d_name);
      }
      if (state->engine_fd >= 0)
         close(state->engine_fd);
      if (state->lease_fd >= 0)
         close(state->lease_fd);
      if (state->lease_parent_fd >= 0)
         close(state->lease_parent_fd);
      if (state->lease_owners_fd >= 0)
         close(state->lease_owners_fd);
      free(state);
      if (valid)
         break; /* At most one 45-second helper/daemon attempt per pass. */
   }
done:
   if (directory)
      closedir(directory);
   if (owners >= 0)
      close(owners);
   if (home >= 0)
      close(home);
   pthread_mutex_unlock(&lock);
   return removed;
#endif
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
/* Docker delegate resources. Go owns creation and runtime posture admission.
 * Ordinary workspaces retain their shell-backed file and execution operations.
 * Strict ACP workspaces hold an immutable container/daemon lease and start a
 * read-only trusted file owner before the external agent. Its bounded private
 * R/W/P/D pipe supplies file bytes; scoped exec preserves producer cancellation
 * and environment. Confirmed retirement releases the held resource artifacts.
 * Failed retirement transfers exact lease custody to the existing reaper. */

#include <aimee/delegates/delegate_backend_docker.h>
#include "util.h"
#include <pthread.h>

#include "aimee.h"      /* MAX_PATH_LEN */
#include "aimee_home.h" /* aimee_home() — resolves the server's UDS path */
#include "cJSON.h"
#include "cli_native_spawn_guard.h"
#include "scoped_file_io.h"

#include <assert.h>

/* Fixed in-container path for the bind-mounted aimee-server UDS. The delegate's
 * only outward channel: `--network none` cuts every IP route, and AIMEE_API_ENDPOINT
 * points the baked-in `aimee` CLI at this socket, so `aimee git_commit`,
 * `aimee web_search`, `aimee memory ...` reach the server (and nothing else). */
#define DELEGATE_SOCK_PATH    "/run/aimee/aimee-http.sock"
#define DELEGATE_OWNER_HELPER "/run/aimee/owner-io"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include "child_environment.h"
#include "log.h"
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/file.h>
#include <dirent.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* RFC 4648 base64 — same as the SSH backend uses. Kept local so this
 * source file stays standalone (no shared b64 header yet). */
static char *docker_exec_environment(const delegate_exec_scope_t *scope)
{
   char *quoted[128] = {0};
   size_t count = 0, bytes = 32;
   if (scope && scope->replace_environment != 0 && scope->replace_environment != 1)
      return NULL;
   for (char *const *entry = scope ? scope->environment : NULL; entry && *entry; entry++)
   {
      const char *equal = strchr(*entry, '=');
      if (!equal || equal == *entry || equal - *entry >= 64 || count == 128 ||
          strlen(*entry) > 65536 - bytes)
         goto failed;
      quoted[count] = shell_quote(*entry);
      if (!quoted[count] || strlen(quoted[count]) >= 65536 - bytes)
         goto failed;
      bytes += strlen(quoted[count]) + 1;
      count++;
   }
   char *prefix = malloc(bytes);
   if (!prefix)
      goto failed;
   size_t used = (size_t)snprintf(prefix, bytes, "%s",
                                  !scope                       ? ""
                                  : scope->replace_environment ? "/usr/bin/env -i "
                                                               : "/usr/bin/env ");
   for (size_t i = 0; i < count; i++)
   {
      used += (size_t)snprintf(prefix + used, bytes - used, "%s ", quoted[i]);
      free(quoted[i]);
   }
   return prefix;
failed:
   for (size_t i = 0; i < 128; i++)
      free(quoted[i]);
   return NULL;
}

static int docker_build_exec_command(const char *container_name, const char *wrapped_script,
                                     int timeout_ms, const delegate_exec_scope_t *scope,
                                     const char *engine, char **out_cmd)
{
   if (out_cmd)
      *out_cmd = NULL;
   if (!container_name || !container_name[0] || !wrapped_script || !out_cmd)
      return -1;

   char *environment = docker_exec_environment(scope);
   char *endpoint = engine && engine[0] ? shell_quote(engine) : strdup("");
   if (!environment || !endpoint)
   {
      free(environment);
      free(endpoint);
      return -1;
   }
   size_t script_len = strlen(wrapped_script);
   size_t b64_cap = ((script_len + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
   {
      free(environment);
      free(endpoint);
      return -1;
   }
   if (util_b64_encode(wrapped_script, script_len, b64, b64_cap) < 0)
   {
      free(b64);
      free(environment);
      free(endpoint);
      return -1;
   }

   /* Put the deadline inside the container as well as around the docker client.
    * Killing only `sh -c docker exec ...` leaves the exec'd process alive in the
    * container, which is exactly how a cancelled Python tool kept consuming a CPU
    * after its durable job was terminal. GNU timeout owns the inner bash process
    * group and terminates its descendants. Fire it 100ms before the outer backend
    * deadline so docker has time to return the bounded result. The delegate image
    * is Ubuntu and already requires coreutils for the base64 transport. */
   int inner_timeout_ms = timeout_ms > 100 ? timeout_ms - 100 : timeout_ms;
   size_t need = strlen(container_name) + b64_cap + strlen(environment) + strlen(endpoint) + 320;
   char *cmd = malloc(need);
   if (!cmd)
   {
      free(b64);
      free(environment);
      free(endpoint);
      return -1;
   }
   const char *bash = scope ? "/bin/bash" : "bash";
   const char *decode = scope ? "/usr/bin/base64" : "base64";
   const char *timer = scope ? "/usr/bin/timeout" : "timeout";
   int n;
   if (inner_timeout_ms > 0)
      n = snprintf(cmd, need,
                   "docker%s%s exec -i %s %s%s -c \"echo '%s' | %s -d | "
                   "%s --signal=TERM --kill-after=1s %d.%03ds %s\"",
                   endpoint[0] ? " --host " : "", endpoint, container_name, environment, bash, b64,
                   decode, timer, inner_timeout_ms / 1000, inner_timeout_ms % 1000, bash);
   else
      n = snprintf(cmd, need, "docker%s%s exec -i %s %s%s -c \"echo '%s' | %s -d | %s\"",
                   endpoint[0] ? " --host " : "", endpoint, container_name, environment, bash, b64,
                   decode, bash);
   free(b64);
   free(environment);
   free(endpoint);
   if (n < 0 || (size_t)n >= need)
   {
      free(cmd);
      return -1;
   }
   *out_cmd = cmd;
   return 0;
}

int delegate_backend_docker_build_exec_command(const char *container_name,
                                               const char *wrapped_script, char **out_cmd)
{
   return docker_build_exec_command(container_name, wrapped_script, 0, NULL, NULL, out_cmd);
}

/* --- container lifecycle --- */

typedef struct
{
   char container_name[128];
   char container_id[65], image_id[72];
   char engine_endpoint[128];
   char docker_binary[MAX_PATH_LEN];
   int retired, engine_fd, lease_fd, lease_parent_fd, lease_owners_fd;
   int artifact_helper_fd, artifact_socket_fd;
   int cleanup_slot, cleanup_failed, cleanup_pending;
   char lease_leaf[65];
   struct stat lease_identity, parent_identity, helper_identity, socket_identity;
   char lease_check[MAX_PATH_LEN];
   cli_native_stdio_t file_owner, forwarder, package_forwarder;
   int launcher_fd, launcher_cwd_fd, file_started;
   int file_ready, file_busy;
   unsigned file_requests;
   long long file_deadline;
   int (*file_validate)(void *);
   void *file_context;
   char prepared_command[4096], prepared_home[4096], prepared_mcp[4096];
   char image[256];
   char workspace_host[MAX_PATH_LEN]; /* host-side mount point */
   char cwd[MAX_PATH_LEN];            /* in-container cwd, defaults to /workspace */
   /* The in-container working directory, and what relative tool paths resolve
    * against. Per-state, not the global resolve_docker_workdir(): a caller-provided
    * tree is mounted at its OWN absolute host path (see docker_build_mounts), so
    * /workspace is not where its files live. Anchoring resolution on a global while
    * the mount moved is how a tool would read the wrong tree and report it as
    * missing. */
   char workdir[MAX_PATH_LEN];
} docker_state_t;

/* Capacity is reserved before strict creation. Failed cleanup transfers its
 * exact held lease here until the existing reaper confirms retirement. */
#define DOCKER_OWNED_CLEANUP_MAX 16
static pthread_mutex_t cleanup_lock = PTHREAD_MUTEX_INITIALIZER;
static docker_state_t *cleanup_slots[DOCKER_OWNED_CLEANUP_MAX];
static int docker_cleanup_reserve(docker_state_t *state)
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
static int docker_owned_capture(docker_state_t *state, const char *const argv[], char **out,
                                size_t maximum, int timeout_ms, int input_fd);
static int server_runs_in_container(void);

static const char *resolve_delegate_egress_bin(void);
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
      if (!removed ||
          docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "socket"),
                                 &state->socket_identity) ||
          docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "helper"),
                                 &state->helper_identity) ||
          docker_source_identity(cJSON_GetObjectItemCaseSensitive(identities, "parent"),
                                 &state->parent_identity))
         removed = no_container = 0;
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
static int docker_cleanup_lease(docker_state_t *state)
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
   failed |= docker_unlink_owned(state->lease_parent_fd, "owner-io", &state->helper_identity, 0);
   failed |=
       docker_unlink_owned(state->lease_parent_fd, "aimee-http.sock", &state->socket_identity, 0);
   if (!failed && state->lease_owners_fd >= 0)
      failed = docker_unlink_owned(state->lease_owners_fd, state->lease_leaf,
                                   &state->parent_identity, AT_REMOVEDIR);
   if (failed)
      LOG_WARN("delegate-sandbox", "owned cleanup artifacts changed; unmatched paths retained");
   else if (state->lease_owners_fd >= 0)
      (void)fsync(state->lease_owners_fd);
}
static void docker_state_destroy(docker_state_t *state)
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
static int docker_reap_pending(void)
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
static int docker_pin_engine(docker_state_t *state)
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

/* The in-container forwarder the package proxy bridges through. */
#define DELEGATE_PKG_PROXY_URL  "http://127.0.0.1:3129"
#define DOCKER_DEFAULT_IMAGE    "ubuntu:22.04"
#define DOCKER_WORKDIR_DEFAULT  "/workspace"
#define DOCKER_PROBE_TIMEOUT_MS 15000

const char *delegate_backend_docker_default_image(void)
{
   return DOCKER_DEFAULT_IMAGE;
}

/* Test seam: production workdir is /workspace inside the container.
 * Unit tests substitute AIMEE_DOCKER_WORKDIR so file ops + mkdir
 * happen under a writable temp dir on the host (the fake-docker
 * fixture runs commands locally, not in a real container). */
static const char *resolve_docker_workdir(void)
{
   const char *override = getenv("AIMEE_DOCKER_WORKDIR");
   if (override && override[0])
      return override;
   return DOCKER_WORKDIR_DEFAULT;
}

/* Pick the docker binary. Tests substitute a fake-docker fixture
 * via AIMEE_DOCKER_BIN. Production runs default to "docker". */
static const char *resolve_docker_bin(void)
{
   const char *override = getenv("AIMEE_DOCKER_BIN");
   if (override && override[0])
      return override;
   return "docker";
}

int delegate_backend_docker_translate_mount_path(const char *container_path,
                                                 const char *mount_table, char *out, size_t outsz)
{
   if (!container_path || container_path[0] != '/' || !mount_table || !out || outsz == 0)
      return -1;

   size_t best_len = 0;
   const char *best_source = NULL;
   size_t best_source_len = 0;
   const char *line = mount_table;
   while (*line)
   {
      const char *end = strchr(line, '\n');
      if (!end)
         end = line + strlen(line);
      const char *tab = memchr(line, '\t', (size_t)(end - line));
      if (tab)
      {
         size_t destination_len = (size_t)(tab - line);
         const char *source = tab + 1;
         size_t source_len = (size_t)(end - source);
         if (source_len && source[source_len - 1] == '\r')
            source_len--;
         if (destination_len > 0 && source_len > 0 && line[0] == '/' && source[0] == '/' &&
             destination_len > best_len && strncmp(container_path, line, destination_len) == 0 &&
             (destination_len == 1 || container_path[destination_len] == '\0' ||
              container_path[destination_len] == '/'))
         {
            best_len = destination_len;
            best_source = source;
            best_source_len = source_len;
         }
      }
      line = *end ? end + 1 : end;
   }

   if (!best_source)
   {
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }
   const char *suffix = best_len == 1 ? container_path : container_path + best_len;
   int n = snprintf(out, outsz, "%.*s%s", (int)best_source_len, best_source, suffix);
   return n < 0 || (size_t)n >= outsz ? -1 : 1;
}

/* Docker bind sources are resolved in the daemon's filesystem namespace. When
 * aimee-server itself runs in Docker, aimee_home() names the path INSIDE this
 * container; passing it straight back to the daemon makes a named-volume path
 * look absent, and Docker silently creates a directory at the requested socket
 * source. Inspect this container's mounts and translate the socket to its host
 * source. A host-native server has no inspectable self container, so it keeps the
 * original path. */
static int docker_host_path_for_container(docker_state_t *owner, const char *container,
                                          const char *container_path, char *out, size_t outsz,
                                          char *table, size_t table_capacity)
{
   if (!container || !container[0])
   {
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }

   const char *argv[] = {resolve_docker_bin(),
                         "inspect",
                         "--format",
                         "{{range .Mounts}}{{printf \"%s\\t%s\\n\" .Destination .Source}}{{end}}",
                         container,
                         NULL};
   char *mounts = NULL;
   int inspected =
       owner ? docker_owned_capture(owner, argv, &mounts, 1 << 20, DOCKER_PROBE_TIMEOUT_MS, -1)
             : safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &mounts, 1 << 20,
                                                 DOCKER_PROBE_TIMEOUT_MS);
   if (inspected != 0)
   {
      free(mounts);
      if (owner)
         return -1;
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }
   if (table && table_capacity)
   {
      if (!mounts || strlen(mounts) >= table_capacity)
      {
         free(mounts);
         return -1;
      }
      memcpy(table, mounts, strlen(mounts) + 1);
   }
   int rc = delegate_backend_docker_translate_mount_path(container_path, mounts ? mounts : "", out,
                                                         outsz);
   free(mounts);
   return rc;
}

static int docker_host_path_for_self(docker_state_t *owner, const char *container_path, char *out,
                                     size_t outsz, char *table, size_t table_capacity)
{
   if (owner && !server_runs_in_container())
   {
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }
   char self[128];
   if (gethostname(self, sizeof(self)) != 0)
      self[0] = '\0';
   self[sizeof(self) - 1] = '\0';
   return docker_host_path_for_container(owner, self, container_path, out, outsz, table,
                                         table_capacity);
}

static int server_runs_in_container(void)
{
   return access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0;
}

/* Run `docker <args>` synchronously, returning the child's exit code.
 * argv must be NULL-terminated. -1 on fork/wait failure. */
static int run_docker(const char *const argv[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      /* Replace argv[0] with the resolved docker binary. */
      const char *bin = resolve_docker_bin();
      /* Build a copy of argv with argv[0] set to bin so the test
       * fixture (which checks argv[0] for "docker") still routes
       * through the override. execvp uses the bin path for lookup
       * but argv[0] for the process name. */
      execvp(bin, (char *const *)argv);
      _exit(127);
   }
   int status = 0;
   if (waitpid(pid, &status, 0) != pid)
      return -1;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The strict handle holds the exact daemon socket inode. Never let a later
 * process-global Docker context/host setting redirect an owned operation. */
static int docker_owned_capture(docker_state_t *state, const char *const argv[], char **out,
                                size_t maximum, int timeout_ms, int input_fd)
{
   if (!state || !state->engine_endpoint[0])
      return safe_exec_capture_cwd_env_fd_timeout(argv, NULL, NULL, out, maximum, timeout_ms,
                                                  input_fd, input_fd >= 0 ? 0 : -1);
   const char *command[128] = {"/bin/sh",
                               "-c",
                               "exec \"$@\" 2>/dev/null",
                               "owned-docker",
                               state->docker_binary,
                               "--host",
                               state->engine_endpoint};
   size_t n = 7;
   for (size_t i = 1; argv && argv[i]; i++)
   {
      if (n + 1 >= sizeof command / sizeof *command)
         return -1;
      command[n++] = argv[i];
   }
   command[n] = NULL;
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent",
                          NULL};
   return safe_exec_capture_cwd_env_fd_timeout(command, NULL, environment, out, maximum, timeout_ms,
                                               input_fd, input_fd >= 0 ? 0 : -1);
}
static int docker_owned_run(docker_state_t *state, const char *const argv[])
{
   char *out = NULL;
   int rc = docker_owned_capture(state, argv, &out, 65536, 30000, -1);
   free(out);
   return rc;
}

/* True when `name` is one of ours. The container-name prefix is matched HERE
 * rather than with `docker ps --filter name=^aimee-delegate-`: the anchored regex
 * is a Docker-ism, and runtimes that treat the name filter as a literal substring
 * (e.g. the LXC-backed docker shim) match nothing at all, silently turning both
 * cleanup paths into no-ops and leaking a container per stale turn. Filtering in C
 * keeps the strict prefix the anchor was there to enforce, on every runtime. */
static int is_delegate_container_name(const char *name)
{
   return name && strncmp(name, "aimee-delegate-", 15) == 0;
}

/* Accept only a bare container id, so a hostile or malformed name can never reach
 * `docker rm`. */
static int is_container_id(const char *id)
{
   size_t len = id ? strlen(id) : 0;
   if (len < 12 || len > 64)
      return 0;
   for (size_t i = 0; i < len; i++)
      if (!isxdigit((unsigned char)id[i]))
         return 0;
   return 1;
}

/* Restart cleanup shares the existing reaper, never the prefix sweep. The
 * only discovery root is this process's configured Aimee home. A live creator,
 * helper or pending owner keeps the journal OFD locked. Each pass advances a
 * bounded directory cursor and attempts at most one qualified journal. */
static int docker_recover_journal(void)
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
         if (valid && state->lease_owners_fd >= 0 && !docker_pin_engine(state) &&
             !docker_cleanup_lease_mode(state, 1))
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

/* Prefix/age cleanup owns only ordinary delegates. Private owner evidence is
 * an exclusion, never an admission verdict: even a broken private mount/startup
 * must remain under its retained journal's exact-container cleanup authority.
 * An unavailable or ambiguous inspect cannot establish ordinary ownership. */
static int docker_is_ordinary_delegate(const char *id)
{
   const char *argv[] = {resolve_docker_bin(),
                         "inspect",
                         "--type",
                         "container",
                         "--format",
                         "{\"mounts\":{{json .Mounts}},\"entrypoint\":{{json .Config.Entrypoint}}}",
                         id,
                         NULL};
   char *out = NULL;
   int ordinary = 0;
   if (safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &out, 65536, DOCKER_PROBE_TIMEOUT_MS) !=
       0)
      goto done;
   cJSON *report = out ? cJSON_ParseWithOpts(out, NULL, 1) : NULL;
   if (!cJSON_IsObject(report) || cJSON_GetArraySize(report) != 2)
      goto parsed;
   const cJSON *mounts = cJSON_GetObjectItemCaseSensitive(report, "mounts");
   const cJSON *entrypoint = cJSON_GetObjectItemCaseSensitive(report, "entrypoint");
   if (!cJSON_IsArray(mounts) || (!cJSON_IsArray(entrypoint) && !cJSON_IsNull(entrypoint)))
      goto parsed;
   const cJSON *item;
   cJSON_ArrayForEach(item, mounts)
   {
      const cJSON *target = cJSON_GetObjectItemCaseSensitive(item, "Destination");
      const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, "Source");
      if (!cJSON_IsObject(item) || !cJSON_IsString(target) || !cJSON_IsString(source))
         goto parsed;
      const char *basename = strrchr(source->valuestring, '/');
      if (!strcmp(target->valuestring, DELEGATE_OWNER_HELPER) ||
          (basename && !strcmp(basename, "/owner-io")))
         goto parsed;
   }
   cJSON_ArrayForEach(item, entrypoint)
   {
      if (!cJSON_IsString(item) || !strcmp(item->valuestring, DELEGATE_OWNER_HELPER))
         goto parsed;
   }
   ordinary = 1;
parsed:
   cJSON_Delete(report);
done:
   free(out);
   if (!ordinary)
      LOG_WARN("delegate-sandbox", "retaining delegate %s: private or unconfirmed cleanup owner",
               id);
   return ordinary;
}

/* Private verifier workers use owner labels and per-worker locks, never
 * the ordinary delegate prefix or age cutoff. Failed reaps retain workers. */
static void reap_private_verifier_orphans(void)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return;
   char directory[MAX_PATH_LEN];
   if (snprintf(directory, sizeof(directory), "%s/verification-workers", home) >=
       (int)sizeof(directory))
      return;
   struct stat st;
   if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode))
      return;
   const char *argv[] = {resolve_delegate_egress_bin(),
                         "reap-verifiers",
                         "--owner-state-dir",
                         directory,
                         "--engine-socket",
                         "/var/run/docker.sock",
                         NULL};
   char *out = NULL;
   if (safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &out, 256, 25000) != 0)
      LOG_WARN("delegate-sandbox", "private verifier orphan cleanup unavailable; workers retained");
   free(out);
}

int delegate_backend_docker_remove_orphans(void)
{
   reap_private_verifier_orphans();
   int recovered = docker_recover_journal();
   const char *list_argv[] = {resolve_docker_bin(), "ps", "-a", "--format",
                              "{{.ID}} {{.Names}}", NULL};
   char *out = NULL;
   if (safe_exec_capture_cwd_env_timeout(list_argv, NULL, NULL, &out, 1 << 20,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(out);
      return -1;
   }
   int removed = recovered;
   char *save = NULL;
   for (char *line = strtok_r(out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      char id[80], name[256];
      if (sscanf(line, "%79s %255s", id, name) != 2)
         continue;
      if (!is_delegate_container_name(name))
         continue;
      if (!is_container_id(id))
      {
         LOG_WARN("delegate-sandbox", "refusing invalid orphan container id from docker: %s", id);
         continue;
      }
      if (!docker_is_ordinary_delegate(id))
         continue;
      const char *remove_argv[] = {resolve_docker_bin(), "rm", "-f", id, NULL};
      if (run_docker(remove_argv) == 0)
         removed++;
      else
         LOG_WARN("delegate-sandbox", "failed to remove orphan container %s", id);
   }
   free(out);
   return removed;
}

/* Days-from-civil (Hinnant) → UTC epoch seconds for a naive Y-M-D H:M:S.
 * Avoids the non-portable timegm(); the caller applies the TZ offset. */
static time_t docker_utc_epoch(int y, int mo, int d, int h, int mi, int s)
{
   y -= mo <= 2;
   int era = (y >= 0 ? y : y - 399) / 400;
   unsigned yoe = (unsigned)(y - era * 400);
   unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
   unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
   long days = (long)era * 146097L + (long)doe - 719468L;
   return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

/* Periodic runtime reap of aged delegate containers.
 *
 * remove_orphans() only runs at startup, and docker_release() only removes the
 * container on the NORMAL turn-completion path. When the heartbeat monitor
 * stale-cancels a delegate job (or the delegate crashes/fails) its running
 * container is left behind — leaking one container per stale/failed turn until the
 * next restart. Over a long uptime these accumulate and fill the image volume.
 *
 * Reap any aimee-delegate-* container older than max_age_secs, chosen ABOVE the
 * in-tool job cap (DEFAULT_IN_TOOL_THRESHOLD_SECS, 1200s) so a live long-running
 * turn is never removed. Returns the count removed, or -1 on a docker error. */
int delegate_backend_docker_reap_aged(int max_age_secs)
{
   int pending_removed = docker_reap_pending() + docker_recover_journal();
   const char *list_argv[] = {resolve_docker_bin(), "ps", "--format",
                              "{{.ID}} {{.Names}} {{.CreatedAt}}", NULL};
   char *out = NULL;
   if (safe_exec_capture_cwd_env_timeout(list_argv, NULL, NULL, &out, 1 << 20,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(out);
      return -1;
   }
   time_t now = time(NULL);
   int removed = pending_removed;
   char *save = NULL;
   for (char *line = strtok_r(out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      /* Line is "<id> <name> " + Go's default CreatedAt: "2006-01-02 15:04:05 -0700 MST". */
      char id[80], name[256];
      int yy, mo, dd, hh, mi, ss;
      char off[8] = "+0000";
      if (sscanf(line, "%79s %255s %d-%d-%d %d:%d:%d %7s", id, name, &yy, &mo, &dd, &hh, &mi, &ss,
                 off) < 8)
         continue;
      if (!is_delegate_container_name(name) || !is_container_id(id))
         continue;
      long off_secs = 0;
      if ((off[0] == '+' || off[0] == '-') && strlen(off) >= 5 && isdigit((unsigned char)off[1]))
      {
         int oh = (off[1] - '0') * 10 + (off[2] - '0');
         int om = (off[3] - '0') * 10 + (off[4] - '0');
         off_secs = (long)(oh * 3600 + om * 60) * (off[0] == '-' ? -1 : 1);
      }
      time_t created = docker_utc_epoch(yy, mo, dd, hh, mi, ss) - off_secs;
      if (created <= 0 || (long)(now - created) < max_age_secs)
         continue;
      if (!docker_is_ordinary_delegate(id))
         continue;
      const char *rm_argv[] = {resolve_docker_bin(), "rm", "-f", id, NULL};
      if (run_docker(rm_argv) == 0)
         removed++;
      else
         LOG_WARN("delegate-sandbox", "failed to reap aged delegate container %s", id);
   }
   free(out);
   return removed;
}

/* Read a linked worktree's `.git` pointer file: "gitdir: <absolute path>".
 * Returns 0 and fills `out` on success. */
static int docker_read_gitlink(const char *gitfile, char *out, size_t outsz)
{
   FILE *f = fopen(gitfile, "r");
   if (!f)
      return -1;
   char line[MAX_PATH_LEN + 16];
   if (!fgets(line, sizeof(line), f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   const char *p = strstr(line, "gitdir:");
   if (!p)
      return -1;
   p += 7;
   while (*p == ' ' || *p == '\t')
      p++;
   size_t n = strlen(p);
   while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
      n--;
   if (!n || p[0] != '/' || n >= outsz)
      return -1; /* must be absolute: a relative gitdir is not one we can mount */
   memcpy(out, p, n);
   out[n] = '\0';
   return 0;
}

/* Add "<src>:<dst>[:ro]" to the state's mount list. */
/* Translate a bind-mount SOURCE from this process's filesystem view to the docker
 * DAEMON's view. Needed when aimee-server itself runs in a container and drives a
 * SIBLING daemon (docker-in-docker / SmoothNAS LXC2Docker): a path like
 * /var/lib/aimee/... exists inside aimee-server's container but NOT on the daemon's
 * host, so binding `src:dst` with the container path silently mounts the wrong (or
 * an empty) directory. The mapping comes from AIMEE_SANDBOX_HOST_MOUNTS, a
 * comma-separated list of `<container-prefix>=<host-prefix>` pairs (longest match
 * wins); the deploy sets it from the plugin's own bind mounts. Unset / no match =>
 * the path is used unchanged, so ordinary same-host deploys are untouched. Only
 * the source is translated — the destination (the path the delegate sees inside
 * the sandbox) is deliberately left as-is. */
/* Discover what a caller-provided tree `real` (already canonical) actually IS.
 *
 * This reads the host filesystem, which is why it stays here: the module decides
 * the container's shape, but it never touches disk and could not answer any of
 * these questions. What it gets back is the answer, as content.
 *
 * A LINKED WORKTREE's .git is a FILE holding `gitdir: <absolute host path>` into
 * the main repo. Mounting the worktree alone leaves that path absent inside the
 * container and every git command fails -- after the delegate has started work,
 * looking like a corrupt repo rather than a bad mount.
 *
 * Fills `repo` with the repository root and `gitdir` with the worktree's git
 * metadata directory. For a PLAIN checkout `repo` is `real` itself and `gitdir`
 * is empty: the tree carries its own .git and there is nothing separate to find.
 * Returns 0, or -1 having logged why. */
/* Bare mirrors have <common>/worktrees/<name>, without a .git component.
 * Follow Git's admin metadata rather than guessing an ancestor from the checkout
 * path. The caller separately verifies the worktree's reverse link below. */
static int docker_bare_worktree_repo(const char *gitdir, char *repo, size_t repo_cap)
{
   char common[MAX_PATH_LEN], path[MAX_PATH_LEN], line[MAX_PATH_LEN];
   if (!realpath(gitdir, common))
      return -1;
   char *name = strrchr(common, '/');
   if (!name || !name[1])
      return -1;
   *name = 0;
   char *worktrees = strrchr(common, '/');
   if (!worktrees || strcmp(worktrees, "/worktrees"))
      return -1;
   *worktrees = 0;
   if (!common[0] || (size_t)snprintf(path, sizeof path, "%s/commondir", gitdir) >= sizeof path)
      return -1;
   FILE *file = fopen(path, "r");
   if (!file)
      return -1;
   int read = fgets(line, sizeof line, file) != NULL;
   fclose(file);
   if (!read)
      return -1;
   line[strcspn(line, "\r\n")] = 0;
   if (strcmp(line, "../.."))
      return -1;
   struct stat metadata;
   const char *parts[] = {"HEAD", "objects", "refs"};
   for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
   {
      if ((size_t)snprintf(path, sizeof path, "%s/%s", common, parts[i]) >= sizeof path ||
          lstat(path, &metadata) ||
          (i == 0 ? !S_ISREG(metadata.st_mode) : !S_ISDIR(metadata.st_mode)))
         return -1;
   }
   return (size_t)snprintf(repo, repo_cap, "%s", common) < repo_cap ? 0 : -1;
}

static int docker_discover_repo(const char *real, char *repo, size_t repo_cap, char *gitdir,
                                size_t gitdir_cap)
{
   repo[0] = '\0';
   gitdir[0] = '\0';

   char gitmark[MAX_PATH_LEN];
   if ((size_t)snprintf(gitmark, sizeof(gitmark), "%s/.git", real) >= sizeof(gitmark))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace path '%s' is too long to check for .git; refusing", real);
      return -1;
   }
   struct stat gst;
   if (lstat(gitmark, &gst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' is not a git checkout (no .git); refusing to bind-mount an "
                "arbitrary host directory into a delegate container",
                real);
      return -1;
   }
   if (S_ISLNK(gst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' has a symlinked .git; refusing to treat it as a checkout", real);
      return -1;
   }

   if (S_ISDIR(gst.st_mode))
   {
      /* Plain checkout: the tree carries its own .git and IS its own repo root. */
      if ((size_t)snprintf(repo, repo_cap, "%s", real) >= repo_cap)
         return -1;
      return 0;
   }

   /* Linked worktree. */
   if (docker_read_gitlink(gitmark, gitdir, gitdir_cap) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': .git is a file but carries no absolute `gitdir:` pointer; "
                "refusing rather than mounting a worktree whose git would be broken",
                real);
      return -1;
   }
   struct stat gdst;
   if (stat(gitdir, &gdst) != 0 || !S_ISDIR(gdst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': gitdir '%s' does not exist on the host; refusing", real, gitdir);
      return -1;
   }
   /* The repo root is what contains that gitdir: <repo>/.git/worktrees/<name>. */
   const char *marker = strstr(gitdir, "/.git/");
   int bare = marker == NULL;
   if (!marker)
   {
      if (docker_bare_worktree_repo(gitdir, repo, repo_cap))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s': gitdir '%s' has no valid common repository; refusing", real,
                   gitdir);
         return -1;
      }
   }
   else
   {
      size_t rlen = (size_t)(marker - gitdir);
      if (rlen == 0 || rlen >= repo_cap)
         return -1;
      memcpy(repo, gitdir, rlen);
      repo[rlen] = '\0';
   }

   /* Is the worktree physically nested under its repo root? A normal delegate
    * sibling worktree (under <repo>/.aimee/worktrees/) is; a WFE per-slice
    * worktree is NOT -- it lives outside the repo, under $AIMEE_HOME/wfe-worktrees.
    * Nesting is not required by the mounts, which bind each path at its own
    * absolute location. What nesting DID provide, for free, was evidence that the
    * worktree belongs to the repo. For the disjoint case we cannot lean on
    * physical containment, so prove the two-way git link explicitly before
    * mounting an out-of-repo tree: the repo's per-worktree admin dir carries a
    * `gitdir` file that must point back at <real>/.git. Together with the forward
    * gitlink read above this defeats a spoofed .git aimed at an unrelated repo.
    * (`real` is already realpath-canonicalized and workspace-authorized.) */
   size_t plen = strlen(repo);
   int nested = (strncmp(real, repo, plen) == 0 && (real[plen] == '/' || real[plen] == '\0'));
   if (bare || !nested)
   {
      char backlink[MAX_PATH_LEN], expect[MAX_PATH_LEN], got[MAX_PATH_LEN] = "";
      if ((size_t)snprintf(backlink, sizeof(backlink), "%s/gitdir", gitdir) >= sizeof(backlink) ||
          (size_t)snprintf(expect, sizeof(expect), "%s/.git", real) >= sizeof(expect))
         return -1;
      FILE *bf = fopen(backlink, "r");
      if (bf)
      {
         if (fgets(got, sizeof(got), bf))
         {
            size_t n = strlen(got);
            while (n && (got[n - 1] == '\n' || got[n - 1] == '\r' || got[n - 1] == ' ' ||
                         got[n - 1] == '\t'))
               got[--n] = '\0';
         }
         fclose(bf);
      }
      if (strcmp(got, expect) != 0)
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is outside repo root '%s' and its worktree backlink '%s' does "
                   "not point back to it (got '%s', want '%s'); refusing",
                   real, repo, backlink, got[0] ? got : "<none>", expect);
         return -1;
      }
   }
   aimee_log(LOG_INFO, "delegate-backend-docker", "%s worktree '%s': repo '%s', gitdir '%s'",
             nested ? "linked" : "disjoint linked (verified backlink)", real, repo, gitdir);
   return 0;
}

/* Render AIMEE_SANDBOX_HOST_MOUNTS into the ONE table format the module reads:
 * "<destination>\t<source>" per line, as `docker inspect` reports mounts.
 *
 * The env var is written "container=host,container2=host2", which is a different
 * encoding of the same mapping. Passing it through unconverted would match
 * nothing, translate nothing, and leave every bind source as an in-container
 * path -- which docker does not reject, it CREATES. The delegate would come up
 * with empty directories where its workspace should be.
 *
 * The env stays the source of truth for workspace sources exactly as before;
 * only the encoding changes. */
static void docker_mount_table(char *out, size_t cap)
{
   out[0] = '\0';
   const char *map = getenv("AIMEE_SANDBOX_HOST_MOUNTS");
   if (!map || !map[0])
      return;

   size_t at = 0;
   const char *p = map;
   while (*p && at + 1 < cap)
   {
      const char *comma = strchr(p, ',');
      size_t plen = comma ? (size_t)(comma - p) : strlen(p);
      const char *eq = memchr(p, '=', plen);
      if (eq)
      {
         size_t clen = (size_t)(eq - p);
         size_t hlen = plen - clen - 1;
         if (clen > 0 && hlen > 0)
         {
            int n = snprintf(out + at, cap - at, "%.*s\t%.*s\n", (int)clen, p, (int)hlen, eq + 1);
            if (n < 0 || (size_t)n >= cap - at)
            {
               /* A truncated table would translate some mounts and not others,
                * which is worse than translating none: the delegate would get a
                * working repo and an empty worktree. */
               out[0] = '\0';
               return;
            }
            at += (size_t)n;
         }
      }
      p = comma ? comma + 1 : p + plen;
   }
}

/* package_access=proxy: copy the static aimee-forwarder into the (running) delegate
 * container and start it detached, so 127.0.0.1:3129 inside the sandbox bridges to the
 * bound aimee UDS where the package forward proxy runs. Best-effort: on failure the
 * delegate still runs; package installs then fail fast (http_proxy -> a dead port is
 * refused immediately, no hang). The forwarder is re-copied/started after every start
 * because a `docker exec -d` process does not survive a container stop. */
/* Remove the forwarder binary from the container so a half-configured proxy delegate
 * can't be left with an unusable-but-present forwarder it could relaunch pointed at an
 * arbitrary UDS path. Best-effort. */
static void docker_pkg_forwarder_strip(const char *container)
{
   const char *rm_argv[] = {
       "docker", "exec", container, "rm", "-f", "/usr/local/bin/aimee-forwarder", NULL};
   (void)run_docker(rm_argv);
}

static int docker_forwarder_start(docker_state_t *state, const char *container, unsigned port)
{
   /* Fixed, canonical ship path — never operator/env-overridable, so nothing can
    * point the copy at an attacker-controlled binary. */
   static const char *const bin = "/usr/local/bin/aimee-forwarder";
   struct stat bst;
   if (stat(bin, &bst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "package-access=proxy but forwarder binary '%s' is missing — sandbox package "
                "installs will fail (ship aimee-forwarder in the aimee-server image)",
                bin);
      return -1;
   }
   char dst[MAX_PATH_LEN + 64];
   snprintf(dst, sizeof(dst), "%s:/usr/local/bin/aimee-forwarder", container);
   const char *cp_argv[] = {"docker", "cp", bin, dst, NULL};
   if ((state ? docker_owned_run(state, cp_argv) : run_docker(cp_argv)) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox", "forwarder `docker cp` into %s failed", container);
      return -1;
   }
   char port_env[48];
   snprintf(port_env, sizeof port_env, "AIMEE_FORWARDER_PORT=%u", port);
   const char *ex_argv[] = {"docker", "exec",
                            "-d",     container,
                            "env",    "AIMEE_FORWARDER_SOCK=" DELEGATE_SOCK_PATH,
                            port_env, "/usr/local/bin/aimee-forwarder",
                            NULL};
   if ((state ? docker_owned_run(state, ex_argv) : run_docker(ex_argv)) != 0)
   {
      /* Copied but not running: strip it rather than leave a relaunchable binary. */
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "starting package forwarder in %s failed — stripping the binary", container);
      docker_pkg_forwarder_strip(container);
      return -1;
   }
   return 0;
}

static void docker_pkg_forwarder_setup(const char *container)
{
   if (docker_forwarder_start(NULL, container, 3129))
      return;
   /* apt does not honour http_proxy through every transport — drop in an explicit
    * proxy config too. Best-effort (needs a root-writable /etc/apt; a non-root
    * delegate still gets the env vars, which cover apt's http/https transports). */
   const char *apt_argv[] = {"docker",
                             "exec",
                             container,
                             "sh",
                             "-c",
                             "mkdir -p /etc/apt/apt.conf.d 2>/dev/null && "
                             "printf 'Acquire::http::Proxy \"http://127.0.0.1:3129\";\\n"
                             "Acquire::https::Proxy \"http://127.0.0.1:3129\";\\n' "
                             "> /etc/apt/apt.conf.d/99aimee-proxy 2>/dev/null || true",
                             NULL};
   (void)run_docker(apt_argv);
   aimee_log(LOG_INFO, "delegate-sandbox",
             "package proxy armed in %s (127.0.0.1:3129 -> aimee UDS; egress via aimee)",
             container);
}

static const char *resolve_delegate_egress_bin(void)
{
   const char *override = getenv("AIMEE_DELEGATE_EGRESS_BIN");
   return override && override[0] ? override : "aimee-delegate-egress";
}

/* C discovers canonical filesystem/socket facts. The Go egress module alone
 * decides the container shape, creates or resumes it, and verifies its complete
 * runtime posture before returning a name. There is deliberately no network or
 * isolation-policy argument at this boundary. */
static int docker_acquire(delegate_backend_t *self, const char *task_id,
                          const delegate_backend_config_t *cfg, void **state_out)
{
   (void)self;
   if (state_out)
      *state_out = NULL;
   if (!task_id || !task_id[0] || !state_out || !cfg || !cfg->workspace || !cfg->workspace[0])
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "a full source workspace is required; refusing an empty sandbox");
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }

   docker_state_t *st = calloc(1, sizeof(*st));
   if (!st)
      return -1;
   st->engine_fd = st->lease_fd = st->lease_parent_fd = st->lease_owners_fd = st->cleanup_slot = -1;
   st->artifact_helper_fd = st->artifact_socket_fd = -1;
   st->file_owner = st->forwarder = st->package_forwarder =
       (cli_native_stdio_t){.in_fd = -1, .out_fd = -1, .pidfd = -1};
   st->launcher_fd = st->launcher_cwd_fd = -1;
   if (snprintf(st->docker_binary, sizeof st->docker_binary, "%s", resolve_docker_bin()) >=
       (int)sizeof st->docker_binary)
   {
      docker_state_destroy(st);
      return -1;
   }
   if (cfg->control_socket && (docker_cleanup_reserve(st) || docker_pin_engine(st)))
   {
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }

   char real[MAX_PATH_LEN] = "", repo[MAX_PATH_LEN] = "", gitdir[MAX_PATH_LEN] = "";
   if (!realpath(cfg->workspace, real))
   {
      aimee_log(LOG_ERROR, "delegate-sandbox", "workspace '%s' does not resolve: %s",
                cfg->workspace, strerror(errno));
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   struct stat wst;
   if (stat(real, &wst) != 0 || !S_ISDIR(wst.st_mode) ||
       docker_discover_repo(real, repo, sizeof(repo), gitdir, sizeof(gitdir)) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "workspace '%s' is not a complete git checkout; refusing", real);
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   if ((size_t)snprintf(st->workspace_host, sizeof(st->workspace_host), "%s", real) >=
           sizeof(st->workspace_host) ||
       (size_t)snprintf(st->workdir, sizeof(st->workdir), "%s", real) >= sizeof(st->workdir))
   {
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   snprintf(st->cwd, sizeof(st->cwd), "%s", st->workdir);
   snprintf(st->image, sizeof(st->image), "%s",
            cfg->image && cfg->image[0] ? cfg->image : delegate_backend_docker_default_image());

   char container_sock[MAX_PATH_LEN + 32] = "";
   char host_sock[MAX_PATH_LEN + 32] = "";
   const char *aimee_h = aimee_home();
   if (cfg->control_socket)
   {
      if ((size_t)snprintf(container_sock, sizeof container_sock, "%s", cfg->control_socket) >=
          sizeof container_sock)
      {
         docker_state_destroy(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
   }
   else if (aimee_h && aimee_h[0])
      snprintf(container_sock, sizeof(container_sock), "%s/aimee-http.sock", aimee_h);
   struct stat socket_stat;
   if (!container_sock[0] || lstat(container_sock, &socket_stat) != 0 ||
       !S_ISSOCK(socket_stat.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "required sole-egress socket '%s' is missing or not a Unix socket",
                container_sock[0] ? container_sock : "<unresolved>");
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   char mount_table[8192];
   docker_mount_table(mount_table, sizeof(mount_table));
   int socket_path_result = docker_host_path_for_self(
       cfg->control_socket ? st : NULL, container_sock, host_sock, sizeof(host_sock),
       cfg->control_socket ? mount_table : NULL, cfg->control_socket ? sizeof mount_table : 0);
   if (socket_path_result < 0 || (server_runs_in_container() && socket_path_result != 1))
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "cannot map sole-egress socket into the Docker daemon namespace; refusing");
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }

   char userflag[64];
   snprintf(userflag, sizeof(userflag), "%u:%u", (unsigned)getuid(), (unsigned)getgid());
   char helper_check[MAX_PATH_LEN + 32] = "", helper_source[MAX_PATH_LEN + 32] = "";
   if (cfg->control_socket)
   {
      const char *check_leaf = strrchr(container_sock, '/'), *source_leaf = strrchr(host_sock, '/');
      if (!cfg->control_helper_sha256 || strlen(cfg->control_helper_sha256) != 64 ||
          !is_container_id(cfg->control_helper_sha256) || !check_leaf || !source_leaf ||
          strcmp(check_leaf + 1, "aimee-http.sock") || strcmp(source_leaf + 1, "aimee-http.sock") ||
          snprintf(helper_check, sizeof helper_check, "%.*s/owner-io",
                   (int)(check_leaf - container_sock),
                   container_sock) >= (int)sizeof helper_check ||
          snprintf(helper_source, sizeof helper_source, "%.*s/owner-io",
                   (int)(source_leaf - host_sock), host_sock) >= (int)sizeof helper_source)
      {
         docker_state_destroy(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
   }

   if (cfg->control_socket)
   {
      char parent_path[MAX_PATH_LEN];
      const char *leaf = strrchr(container_sock, '/');
      if (!leaf ||
          snprintf(parent_path, sizeof parent_path, "%.*s", (int)(leaf - container_sock),
                   container_sock) >= (int)sizeof parent_path ||
          snprintf(st->lease_check, sizeof st->lease_check, "%s/lease.jsonl", parent_path) >=
              (int)sizeof st->lease_check)
      {
         docker_state_destroy(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
      int parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat status;
      int valid = parent >= 0 && !fstat(parent, &status) && status.st_uid == getuid() &&
                  (status.st_mode & 0777) == 0700;
      if (valid)
         st->lease_fd = openat(parent, "lease.jsonl",
                               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#if defined(__linux__)
      if (st->lease_fd >= 0 && flock(st->lease_fd, LOCK_EX | LOCK_NB))
      {
         close(st->lease_fd);
         st->lease_fd = -1;
      }
#endif
      if (st->lease_fd >= 0)
         st->lease_parent_fd = fcntl(parent, F_DUPFD_CLOEXEC, 10);
      valid = valid && st->lease_fd >= 0 && st->lease_parent_fd >= 0 && !fsync(parent) &&
              !fstat(st->lease_fd, &st->lease_identity) && !fstat(parent, &st->parent_identity) &&
              (st->artifact_helper_fd =
                   openat(parent, "owner-io", O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) >= 0 &&
              !fstat(st->artifact_helper_fd, &st->helper_identity) &&
              S_ISREG(st->helper_identity.st_mode);
#if defined(__linux__)
      if (valid)
      {
         st->artifact_socket_fd =
             openat(parent, "aimee-http.sock", O_PATH | O_NOFOLLOW | O_CLOEXEC);
         valid = st->artifact_socket_fd >= 0 &&
                 !fstat(st->artifact_socket_fd, &st->socket_identity) &&
                 S_ISSOCK(st->socket_identity.st_mode);
      }
#else
      valid = 0;
#endif
      if (valid)
      {
         const char *nonce = strrchr(parent_path, '/');
         char owners_path[MAX_PATH_LEN];
         struct stat named;
         valid = nonce && strlen(nonce + 1) == 64 &&
                 snprintf(owners_path, sizeof owners_path, "%.*s", (int)(nonce - parent_path),
                          parent_path) < (int)sizeof owners_path &&
                 (st->lease_owners_fd =
                      open(owners_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) >= 0 &&
                 !fstatat(st->lease_owners_fd, nonce + 1, &named, AT_SYMLINK_NOFOLLOW) &&
                 named.st_dev == st->parent_identity.st_dev &&
                 named.st_ino == st->parent_identity.st_ino;
         if (valid)
         {
            snprintf(st->lease_leaf, sizeof st->lease_leaf, "%s", nonce + 1);
            /* Parent descriptors remain held through pending reaping. */
         }
      }
      if (parent >= 0)
         close(parent);
      if (!valid)
      {
         docker_state_destroy(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
   }
   const char *argv[52];
   int n = 0;
   argv[n++] = resolve_delegate_egress_bin();
   argv[n++] = "acquire";
   if (cfg->control_socket)
   {
      argv[n++] = "--receipt-json";
      argv[n++] = "--lease-fd";
      argv[n++] = "3";
      argv[n++] = "--lease-check";
      argv[n++] = st->lease_check;
      argv[n++] = "--engine-endpoint";
      argv[n++] = st->engine_endpoint;
      argv[n++] = "--helper-check";
      argv[n++] = helper_check;
      argv[n++] = "--helper-source";
      argv[n++] = helper_source;
      argv[n++] = "--helper-sha256";
      argv[n++] = cfg->control_helper_sha256;
   }
   argv[n++] = "--docker";
   argv[n++] = resolve_docker_bin();
   argv[n++] = "--task";
   argv[n++] = task_id;
   argv[n++] = "--image";
   argv[n++] = st->image;
   argv[n++] = "--workdir";
   argv[n++] = st->workdir;
   argv[n++] = "--repo-root";
   argv[n++] = repo;
   argv[n++] = "--worktree";
   argv[n++] = real;
   if (gitdir[0])
   {
      argv[n++] = "--gitdir";
      argv[n++] = gitdir;
   }
   argv[n++] = "--user";
   argv[n++] = userflag;
   argv[n++] = "--socket-source";
   argv[n++] = host_sock;
   argv[n++] = "--socket-check";
   argv[n++] = container_sock;
   if (!cfg->workspace_read_only)
      argv[n++] = "--writes-allowed";
   if (cfg->pkg_proxy)
      argv[n++] = "--proxy";
   if (mount_table[0])
   {
      argv[n++] = "--mount-table";
      argv[n++] = mount_table;
   }
   assert(n < (int)(sizeof(argv) / sizeof(argv[0])));
   argv[n] = NULL;

   char *out = NULL;
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent",
                          NULL};
   int rc = cfg->control_socket
                ? safe_exec_capture_cwd_env_fd_timeout(argv, NULL, environment, &out, 8192, 70000,
                                                       st->lease_fd, 3)
                : safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &out, 4096, 70000);
   if (rc != 0 || !out)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "Go egress module could not establish and verify delegate %s; refusing", task_id);
      free(out);
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   size_t len = strlen(out);
   while (len && isspace((unsigned char)out[len - 1]))
      out[--len] = '\0';
   cJSON *receipt = cfg->control_socket ? cJSON_Parse(out) : NULL;
   const char *name = cfg->control_socket
                          ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "name"))
                          : out;
   if (cfg->control_socket)
   {
      const char *engine =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "engine_endpoint"));
      const char *helper =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "helper_source"));
      const char *helper_target =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "helper_target"));
      const char *helper_sha =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "helper_sha256"));
      const char *startup =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "trusted_startup"));
      const char *schema =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "schema"));
      const char *id =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "container_id"));
      const char *image =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "image_id"));
      const char *workdir =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "workdir"));
      const char *source =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "control_socket_source"));
      const char *target =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receipt, "control_socket_target"));
      if (!engine || strcmp(engine, st->engine_endpoint) || !helper ||
          strcmp(helper, helper_source) || !helper_target ||
          strcmp(helper_target, DELEGATE_OWNER_HELPER) || !helper_sha ||
          strcmp(helper_sha, cfg->control_helper_sha256) || !startup ||
          strcmp(startup, "owner-io-idle-guard-v1") ||
          !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "fresh_container")) || !schema ||
          strcmp(schema, "aimee.container-acquire.v1") || !id || strlen(id) != 64 ||
          !is_container_id(id) || !image || strlen(image) != 71 || strncmp(image, "sha256:", 7) ||
          !is_container_id(image + 7) || !workdir || strcmp(workdir, st->workdir) || !source ||
          strcmp(source, host_sock) || !target || strcmp(target, DELEGATE_SOCK_PATH) ||
          !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "verified_posture")))
      {
         cJSON_Delete(receipt);
         free(out);
         docker_state_destroy(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
      snprintf(st->container_id, sizeof st->container_id, "%s", id);
      snprintf(st->image_id, sizeof st->image_id, "%s", image);
   }
   if (!name)
      name = "";
   while (*name && isspace((unsigned char)*name))
      name++;
   if (!name[0] || strpbrk(name, " \t\r\n") ||
       (size_t)snprintf(st->container_name, sizeof(st->container_name), "%s", name) >=
           sizeof(st->container_name))
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "Go egress module returned an invalid container name");
      free(out);
      cJSON_Delete(receipt);
      docker_state_destroy(st);
      return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
   }
   cJSON_Delete(receipt);
   free(out);

   if (cfg->pkg_proxy && !cfg->control_socket)
      docker_pkg_forwarder_setup(st->container_name);
   *state_out = st;
   return 0;
}

static int delegate_backend_docker_owned_identity(void *state, char container_id[65],
                                                  char image_id[72])
{
   docker_state_t *owner = state;
   if (!owner || owner->retired || strlen(owner->container_id) != 64 ||
       strlen(owner->image_id) != 71 || !container_id || !image_id)
      return -1;
   memcpy(container_id, owner->container_id, 65);
   memcpy(image_id, owner->image_id, 72);
   return 0;
}
static int delegate_backend_docker_owned_transport(void *state, const char **binary,
                                                   const char **engine_endpoint)
{
   docker_state_t *owner = state;
   char id[65], image[72];
   if (!binary || !engine_endpoint || delegate_backend_docker_owned_identity(state, id, image) ||
       owner->engine_fd < 0 || !owner->engine_endpoint[0] || !owner->docker_binary[0])
      return -1;
   *binary = owner->docker_binary;
   *engine_endpoint = owner->engine_endpoint;
   return 0;
}
static int docker_file_live(docker_state_t *owner, long long deadline)
{
   return owner && !owner->retired && owner->file_validate &&
          !owner->file_validate(owner->file_context) && util_now_ms() < deadline;
}
static int docker_transport_io(docker_state_t *owner, const cli_native_stdio_t *transport,
                               int writing, char *bytes, size_t length, long long deadline)
{
   int fd = writing ? transport->in_fd : transport->out_fd;
   size_t offset = 0;
   while (offset < length)
   {
      if (!docker_file_live(owner, deadline))
         return -1;
      long long remaining = deadline - util_now_ms();
      struct pollfd pending = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
      int ready = poll(&pending, 1, remaining > 100 ? 100 : (int)remaining);
      if (ready < 0 && errno == EINTR)
         continue;
      if (ready < 0 || (pending.revents & POLLNVAL))
         return -1;
      if (!ready)
         continue;
      ssize_t n = writing ? write(fd, bytes + offset, length - offset)
                          : read(fd, bytes + offset, length - offset);
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      if (n <= 0)
         return -1;
      offset += (size_t)n;
   }
   return 0;
}
static int docker_transport_line(docker_state_t *owner, const cli_native_stdio_t *transport,
                                 char *line, size_t capacity, long long deadline)
{
   for (size_t i = 0; i + 1 < capacity; i++)
   {
      if (docker_transport_io(owner, transport, 0, line + i, 1, deadline))
         return -1;
      if (line[i] == '\n')
      {
         line[i] = 0;
         return 0;
      }
   }
   return -1;
}
static int delegate_backend_docker_file_owner_start(void *state, int executable_fd, int cwd_fd,
                                                    const char *selected_command,
                                                    int (*validate)(void *), void *context,
                                                    long long deadline_ms)
{
   docker_state_t *owner = state;
   char id[65], image[72];
   if (delegate_backend_docker_owned_identity(state, id, image) || owner->file_ready ||
       owner->file_started || owner->file_owner.pid || !selected_command || !selected_command[0] ||
       !validate || validate(context) || util_now_ms() >= deadline_ms)
      return -1;
   owner->file_started = 1;
   owner->launcher_fd = cli_native_pin_fd(executable_fd);
   owner->launcher_cwd_fd = cli_native_pin_fd(cwd_fd);
   if (owner->launcher_fd < 0 || owner->launcher_cwd_fd < 0)
      return -1;
   owner->file_validate = validate;
   owner->file_context = context;
   owner->file_deadline = deadline_ms;
   char *argv[] = {owner->docker_binary,
                   "--host",
                   owner->engine_endpoint,
                   "exec",
                   "-i",
                   "--workdir",
                   owner->workdir,
                   owner->container_id,
                   DELEGATE_OWNER_HELPER,
                   "--file-serve",
                   owner->workdir,
                   (char *)selected_command,
                   NULL};
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent",
                          NULL};
   if (cli_native_stdio_spawn(executable_fd, cwd_fd, argv, environment, validate, context,
                              &owner->file_owner))
      return -1;
   char ready[128], trailing;
   size_t command_length, home_length, mcp_length;
   if (docker_transport_line(owner, &owner->file_owner, ready, sizeof ready, deadline_ms) ||
       sscanf(ready, "raw-owner-v1 ready %zu %zu %zu %c", &command_length, &home_length,
              &mcp_length, &trailing) != 3 ||
       !command_length || command_length >= sizeof owner->prepared_command || !home_length ||
       home_length >= sizeof owner->prepared_home || mcp_length >= sizeof owner->prepared_mcp ||
       docker_transport_io(owner, &owner->file_owner, 0, owner->prepared_command, command_length,
                           deadline_ms) ||
       docker_transport_io(owner, &owner->file_owner, 0, owner->prepared_home, home_length,
                           deadline_ms) ||
       (mcp_length && docker_transport_io(owner, &owner->file_owner, 0, owner->prepared_mcp,
                                          mcp_length, deadline_ms)))
   {
      cli_native_stdio_close(&owner->file_owner);
      return -1;
   }
   owner->prepared_command[command_length] = owner->prepared_home[home_length] =
       owner->prepared_mcp[mcp_length] = 0;
   if (strlen(owner->prepared_command) != command_length ||
       strlen(owner->prepared_home) != home_length || strlen(owner->prepared_mcp) != mcp_length ||
       owner->prepared_command[0] != '/' || owner->prepared_home[0] != '/' ||
       (mcp_length && owner->prepared_mcp[0] != '/'))
   {
      cli_native_stdio_close(&owner->file_owner);
      return -1;
   }
   owner->file_ready = 1;
   return 0;
}
static int delegate_backend_docker_owned_prepared(void *state, const char **command,
                                                  const char **home, const char **mcp_command)
{
   docker_state_t *owner = state;
   if (!owner || !owner->file_ready || owner->retired || !command || !home || !mcp_command)
      return -1;
   *command = owner->prepared_command;
   *home = owner->prepared_home;
   *mcp_command = owner->prepared_mcp;
   return 0;
}
static int docker_owned_file_request(void *state, const char *path, char operation,
                                     const char *content, size_t length, int timeout_ms, char **out,
                                     size_t *out_length, int *unknown)
{
   docker_state_t *owner = state;
   int writing = operation == 'W' || operation == 'P';
   if (out)
      *out = NULL;
   if (out_length)
      *out_length = 0;
   if (unknown)
      *unknown = 0;
   if (!owner || !out || !out_length || !unknown || !owner->file_ready || owner->file_busy ||
       owner->file_requests >= 4096 ||
       (operation != 'R' && operation != 'W' && operation != 'P' && operation != 'D') || !path ||
       !path[0] || strlen(path) > 4095 || length > SCOPED_FILE_IO_MAX || (!content && length) ||
       (!writing && length))
      return -1;
   owner->file_busy = 1;
   owner->file_requests++;
   long long deadline = owner->file_deadline, now = util_now_ms();
   if (timeout_ms > 0 && now + timeout_ms < deadline)
      deadline = now + timeout_ms;
   char header[96], trailing;
   int n =
       snprintf(header, sizeof header, "raw-file-v1 %c %zu %zu\n", operation, strlen(path), length);
   int rc = -1, status;
   size_t received;
   if (n < 0 || (size_t)n >= sizeof header ||
       docker_transport_io(owner, &owner->file_owner, 1, header, (size_t)n, deadline) ||
       docker_transport_io(owner, &owner->file_owner, 1, (char *)path, strlen(path), deadline) ||
       (length &&
        docker_transport_io(owner, &owner->file_owner, 1, (char *)content, length, deadline)))
      goto failed;
   *unknown = writing; /* A lost response cannot prove no admitted write occurred. */
   if (docker_transport_line(owner, &owner->file_owner, header, sizeof header, deadline) ||
       sscanf(header, "raw-file-v1 %d %zu %c", &status, &received, &trailing) != 2 || status < 0 ||
       status > 2 || received > SCOPED_FILE_IO_MAX || ((writing || status) && received))
      goto failed;
   if (status)
   {
      *unknown = status == 2;
      owner->file_busy = 0;
      return -1;
   }
   if (!writing)
   {
      *out = malloc(received + 1);
      if (!*out || docker_transport_io(owner, &owner->file_owner, 0, *out, received, deadline))
         goto failed;
      (*out)[received] = 0;
      *out_length = received;
   }
   if (!docker_file_live(owner, deadline))
      goto failed;
   *unknown = 0;
   rc = 0;
   owner->file_busy = 0;
   return rc;
failed:
   owner->file_ready = 0; /* No retry/resynchronization after an uncertain frame. */
   owner->file_busy = 0;
   free(*out);
   *out = NULL;
   *out_length = 0;
   return -1;
}

static int delegate_backend_docker_owned_file(void *state, const char *path, int writing,
                                              const char *content, size_t length, int timeout_ms,
                                              char **out, size_t *out_length, int *unknown)
{
   return docker_owned_file_request(state, path, writing ? 'W' : 'R', content, length, timeout_ms,
                                    out, out_length, unknown);
}

static int delegate_backend_docker_owned_forwarder(void *state, unsigned port)
{
   docker_state_t *owner = state;
   char id[65], image[72];
   cli_native_stdio_t *process = port == 3129 ? &owner->package_forwarder : &owner->forwarder;
   if ((port != 3131 && port != 3129) || delegate_backend_docker_owned_identity(state, id, image) ||
       !owner->file_ready || process->pid)
      return -1;
   char *argv[] = {owner->docker_binary,
                   "--host",
                   owner->engine_endpoint,
                   "exec",
                   "-i",
                   owner->container_id,
                   DELEGATE_OWNER_HELPER,
                   "--owned-forwarder",
                   port == 3129 ? "3129" : "3131",
                   DELEGATE_SOCK_PATH,
                   NULL};
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent",
                          NULL};
   if (cli_native_stdio_spawn(owner->launcher_fd, owner->launcher_cwd_fd, argv, environment,
                              owner->file_validate, owner->file_context, process))
      return -1;
   char ready[64];
   if (docker_transport_line(owner, process, ready, sizeof ready, owner->file_deadline) ||
       strcmp(ready, "raw-forwarder-v1 ready"))
   {
      cli_native_stdio_close(process);
      return -1;
   }
   return 0;
}

static int delegate_backend_docker_owned_retire(void *state)
{
   docker_state_t *owner = state;
   char id[65], image[72];
   if (delegate_backend_docker_owned_identity(state, id, image))
      return owner && owner->retired ? 0 : -1;
   owner->file_ready = 0;
   cli_native_stdio_close(&owner->file_owner);
   cli_native_stdio_close(&owner->forwarder);
   cli_native_stdio_close(&owner->package_forwarder);
   owner->file_validate = NULL;
   owner->file_context = NULL;
   if (owner->lease_fd >= 0)
      return docker_cleanup_lease(owner);
   const char *remove[] = {resolve_docker_bin(), "rm", "-f", id, NULL};
   if (docker_owned_run(owner, remove))
      return -1;
   char filter[80];
   snprintf(filter, sizeof filter, "id=%s", id);
   const char *inspect[] = {resolve_docker_bin(), "ps",   "-aq", "--no-trunc",
                            "--filter",           filter, NULL};
   char *remaining = NULL;
   int rc = docker_owned_capture(owner, inspect, &remaining, 4096, 15000, -1);
   int absent = !rc && (!remaining || !remaining[strspn(remaining, " \t\r\n")]);
   free(remaining);
   if (!absent)
      return -1;
   owner->retired = 1;
   return 0;
}

static void docker_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   if (!state)
      return;
   docker_state_t *st = state;
   if (st->container_id[0])
   {
      if (delegate_backend_docker_owned_retire(st))
         LOG_ERROR("delegate-sandbox", "owned container retirement was not verified");
      docker_state_destroy(st);
      return;
   }
   const char *identity = st->container_name;
   if (identity[0] && !st->retired)
   {
      if (hibernate)
      {
         /* Stop only; container persists for next acquire. */
         const char *argv[] = {"docker", "stop", identity, NULL};
         (void)run_docker(argv);
      }
      else
      {
         /* Force-remove (kills if running, removes container). */
         const char *argv[] = {"docker", "rm", "-f", identity, NULL};
         (void)run_docker(argv);
      }
   }
   docker_state_destroy(st);
}

static int docker_exec_scoped(delegate_backend_t *self, void *state, const char *command,
                              int timeout_ms, const delegate_exec_scope_t *scope,
                              delegate_exec_result_t *result_out)
{
   (void)self;
   if (!state || ((docker_state_t *)state)->retired || !command || !result_out)
      return -1;
   docker_state_t *st = state;
   long long start = util_now_ms();
   result_out->exit_code = -1;

   /* Prefix `cd <cwd> &&` when set_cwd has been called. The container
    * default cwd is /workspace (see -w flag at create); set_cwd lets
    * the operator land elsewhere across exec calls. */
   char *prefixed = NULL;
   const char *to_run = command;
   if (st->cwd[0] && strcmp(st->cwd, resolve_docker_workdir()) != 0)
   {
      char *quoted_cwd = shell_quote(st->cwd);
      if (!quoted_cwd)
      {
         return -1;
      }
      size_t need = strlen(quoted_cwd) + strlen(command) + 16;
      prefixed = malloc(need);
      if (!prefixed)
      {
         free(quoted_cwd);
         return -1;
      }
      snprintf(prefixed, need, "cd %s && %s", quoted_cwd, command);
      free(quoted_cwd);
      to_run = prefixed;
   }

   /* Build the docker shell command via the iter-37 b64 helper. */
   char *docker_cmd = NULL;
   if (docker_build_exec_command(st->container_id[0] ? st->container_id : st->container_name,
                                 to_run, timeout_ms, scope, st->engine_endpoint,
                                 &docker_cmd) != 0 ||
       !docker_cmd)
   {
      free(prefixed);
      return -1;
   }
   free(prefixed);

   /* If AIMEE_DOCKER_BIN is set, swap the leading "docker" in the
    * built command for the override binary. Production runs leave
    * the env unset and the leading "docker" wins via PATH. */
   const char *bin_override = getenv("AIMEE_DOCKER_BIN");
   if (bin_override && bin_override[0] && strncmp(docker_cmd, "docker ", 7) == 0)
   {
      size_t bin_len = strlen(bin_override);
      size_t tail_len = strlen(docker_cmd) - 6; /* "docker" is 6 chars */
      char *swapped = malloc(bin_len + tail_len + 1);
      if (!swapped)
      {
         free(docker_cmd);
         return -1;
      }
      {
         memcpy(swapped, bin_override, bin_len);
         memcpy(swapped + bin_len, docker_cmd + 6, tail_len + 1);
         free(docker_cmd);
         docker_cmd = swapped;
      }
   }

   int out_pipe[2] = {-1, -1};
   int err_pipe[2] = {-1, -1};
   if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
   {
      if (out_pipe[0] >= 0)
         close(out_pipe[0]);
      if (out_pipe[1] >= 0)
         close(out_pipe[1]);
      if (err_pipe[0] >= 0)
         close(err_pipe[0]);
      if (err_pipe[1] >= 0)
         close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }

   int *descriptors[] = {&out_pipe[0], &out_pipe[1], &err_pipe[0], &err_pipe[1]};
   for (size_t i = 0; i < 4; i++)
   {
      int pinned = fcntl(*descriptors[i], F_DUPFD_CLOEXEC, 10);
      if (pinned < 0)
      {
         for (size_t j = 0; j < 4; j++)
            close(*descriptors[j]);
         free(docker_cmd);
         return -1;
      }
      close(*descriptors[i]);
      *descriptors[i] = pinned;
   }
   pid_t pid = fork();
   if (pid < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }
   if (pid == 0)
   {
      (void)setpgid(0, 0);
      int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
      int input = null_input < 0 ? -1 : fcntl(null_input, F_DUPFD_CLOEXEC, 10);
      if (null_input >= 0)
         close(null_input);
      if (input < 0 || dup2(input, STDIN_FILENO) < 0)
         _exit(126);
      close(input);
      if ((scope && child_environment_install(scope->environment, scope->replace_environment)) ||
          dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(err_pipe[1], STDERR_FILENO) < 0)
         _exit(126);
      /* Original pipe descriptors are all >=10 and cannot close mapped stdio. */
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      execl("/bin/sh", "sh", "-c", docker_cmd, (char *)NULL);
      _exit(127);
   }
   free(docker_cmd);
   close(out_pipe[1]);
   close(err_pipe[1]);

   (void)setpgid(pid, pid);
   int nonblocking = 1;
   for (int i = 0; i < 2; i++)
   {
      int fd = i ? err_pipe[0] : out_pipe[0];
      int flags = fcntl(fd, F_GETFL);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
         nonblocking = 0;
   }
   long long deadline = timeout_ms > 0 ? start + timeout_ms : 0;
   size_t out_off = 0, err_off = 0;
   int open_stream[2] = {1, 1}, reaped = 0, status = 0, interrupted = !nonblocking;
   while (!interrupted && (open_stream[0] || open_stream[1] || !reaped))
   {
      if (!reaped)
      {
         pid_t done = waitpid(pid, &status, WNOHANG);
         if (done == pid)
            reaped = 1;
         else if (done < 0 && errno != EINTR)
         {
            interrupted = 1;
            break;
         }
      }
      if (!open_stream[0] && !open_stream[1] && reaped)
         break;
      if ((scope && scope->progress && scope->progress(scope->context)) ||
          (deadline && util_now_ms() >= deadline))
      {
         interrupted = 1;
         break;
      }
      struct pollfd streams[2] = {{open_stream[0] ? out_pipe[0] : -1, POLLIN, 0},
                                  {open_stream[1] ? err_pipe[0] : -1, POLLIN, 0}};
      int ready = poll(streams, 2, 100);
      if (ready < 0)
      {
         if (errno == EINTR)
            continue;
         interrupted = 1;
         break;
      }
      for (int i = 0; i < 2; i++)
      {
         if (!streams[i].revents)
            continue;
         if (streams[i].revents & POLLNVAL)
         {
            interrupted = 1;
            break;
         }
         char discard[4096];
         char *buffer = i ? result_out->stderr_buf : result_out->stdout_buf;
         size_t capacity = i ? result_out->stderr_cap : result_out->stdout_cap;
         size_t *offset = i ? &err_off : &out_off;
         size_t available = buffer && *offset + 1 < capacity ? capacity - *offset - 1 : 0;
         ssize_t count = read(streams[i].fd, available ? buffer + *offset : discard,
                              available ? available : sizeof discard);
         if (count > 0)
         {
            if (available)
               *offset += (size_t)count;
         }
         else if (!count)
            open_stream[i] = 0;
         else if (errno != EAGAIN && errno != EINTR)
         {
            interrupted = 1;
            break;
         }
      }
      if (interrupted)
         break;
   }
   close(out_pipe[0]);
   close(err_pipe[0]);
   if (interrupted)
   {
      /* Joining the local transport is NOT proof the remote exec stopped.
       * The owning runtime must retire its immutable container before terminal. */
      kill(-pid, SIGTERM);
      kill(pid, SIGTERM);
      long long grace = util_now_ms() + 200;
      while (!reaped && util_now_ms() < grace)
      {
         pid_t done = waitpid(pid, &status, WNOHANG);
         if (done == pid || (done < 0 && errno != EINTR))
            reaped = 1;
         else
            (void)poll(NULL, 0, 10);
      }
      kill(-pid, SIGKILL);
      if (!reaped)
      {
         kill(pid, SIGKILL);
         while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
         {
         }
      }
   }

   /* Terminate where the OUTPUT ends, not at the end of the buffer.
    *
    * The caller's buffer is malloc'd and uninitialized, and callers read it with
    * strlen() -- docker_read_file does exactly that before slicing. Terminating
    * only the final byte leaves whatever the allocator last had in that arena
    * sitting between the real output and that NUL, so strlen() walks straight
    * into it and the delegate is handed its file with heap garbage appended.
    *
    * It read correctly for as long as these allocations happened to be fresh
    * pages, which are zero. Any allocation and free of comparable size beforehand
    * is enough to expose it, and the result is silent: a file the delegate reads
    * comes back longer than it is, with plausible-looking bytes on the end.
    *
    * The final byte stays terminated as a backstop for a truncated capture. */
   if (result_out->stdout_buf && result_out->stdout_cap > 0)
   {
      result_out
          ->stdout_buf[out_off < result_out->stdout_cap ? out_off : result_out->stdout_cap - 1] =
          '\0';
      result_out->stdout_buf[result_out->stdout_cap - 1] = '\0';
   }
   if (result_out->stderr_buf && result_out->stderr_cap > 0)
   {
      result_out
          ->stderr_buf[err_off < result_out->stderr_cap ? err_off : result_out->stderr_cap - 1] =
          '\0';
      result_out->stderr_buf[result_out->stderr_cap - 1] = '\0';
   }

   result_out->exit_code = !interrupted && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
   result_out->latency_ms = (int)(util_now_ms() - start);
   return interrupted || !WIFEXITED(status) ? -1 : 0;
}
static int docker_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                       delegate_exec_result_t *result_out)
{
   return docker_exec_scoped(self, state, command, timeout_ms, NULL, result_out);
}

/* Workspace path validation: reject absolute paths and any '..' segment
 * so the in-container workspace sandbox is honest. Resolves `rel`
 * against the container's WORKDIR (/workspace by default) so the same
 * path the caller hands in lands in the same place across exec calls. */
/* Collapse harmless spelling without touching the host filesystem. Keep a
 * terminal directory marker: reading/writing file/ or file/. must still fail. */
static int docker_normalize_workspace_path(char *path, size_t capacity)
{
   size_t length = strlen(path), read_at = 0, used = 1;
   int directory =
       length && (path[length - 1] == '/' || (length >= 2 && !strcmp(path + length - 2, "/.")));
   if (!length || path[0] != '/' || capacity < 2)
      return -1;
   while (read_at < length)
   {
      while (read_at < length && path[read_at] == '/')
         read_at++;
      size_t start = read_at;
      while (read_at < length && path[read_at] != '/')
         read_at++;
      size_t n = read_at - start;
      if (!n || (n == 1 && path[start] == '.'))
         continue;
      if (n == 2 && path[start] == '.' && path[start + 1] == '.')
         return -1;
      if (used + n + (used > 1) >= capacity)
         return -1;
      if (used > 1)
         path[used++] = '/';
      memmove(path + used, path + start, n);
      used += n;
   }
   if (directory && used > 1)
   {
      if (used + 1 >= capacity)
         return -1;
      path[used++] = '/';
   }
   path[used] = 0;
   return 0;
}

static int docker_resolve_in_workspace(const docker_state_t *st, const char *rel, char *out,
                                       size_t outsz)
{
   if (!st || !rel || !out || outsz == 0)
      return -1;
   /* Reject any parent-traversal segment outright — it could escape the workspace
    * whether the input is relative or absolute. */
   const char *p = rel;
   while (*p)
   {
      const char *seg = p;
      while (*p && *p != '/')
         p++;
      size_t len = (size_t)(p - seg);
      if (len == 2 && seg[0] == '.' && seg[1] == '.')
         return -1;
      if (*p == '/')
         p++;
   }
   const char *workdir = st->workdir[0] ? st->workdir : resolve_docker_workdir();
   if (rel[0] == '/')
   {
      /* Accept an ABSOLUTE path only when it is within the workspace root. The slice
       * worktree is bind-mounted path-identically, so such a path is already the valid
       * in-container path. The native file tools (tool_read_file/tool_write_file) resolve
       * to an absolute path via the thread cwd BEFORE calling the provider, so without
       * this branch every in-workspace read/write is wrongly rejected (bash still works
       * because it runs a raw `cd && ...` command that never reaches this resolver) — the
       * live symptom was container delegates hitting "cannot open"/"cannot write" on their
       * own worktree. A path OUTSIDE the workspace stays refused: the container must not
       * reach the host tree. */
      size_t wlen = strlen(workdir);
      if (strncmp(rel, workdir, wlen) == 0 && (rel[wlen] == '/' || rel[wlen] == '\0'))
      {
         if (snprintf(out, outsz, "%s", rel) >= (int)outsz)
            return -1;
         return docker_normalize_workspace_path(out, outsz);
      }
      return -1;
   }
   if (snprintf(out, outsz, "%s/%s", workdir, rel) >= (int)outsz)
      return -1;
   return docker_normalize_workspace_path(out, outsz);
}

static int docker_read_file(delegate_backend_t *self, void *state, const char *p, int offset,
                            int limit, char **out)
{
   if (out)
      *out = NULL;
   if (!state || !p || !out || offset < 0 || limit < 0)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN], *buf = NULL;
   if (docker_resolve_in_workspace(st, p, abs, sizeof abs))
      return -1;
   size_t cap = limit > 0 ? (size_t)limit : SCOPED_FILE_IO_MAX, n = 0;
   if (st->engine_fd >= 0)
   {
      int unknown = 0;
      if (docker_owned_file_request(st, abs, 'R', NULL, 0, 30000, &buf, &n, &unknown) ||
          memchr(buf, 0, n))
      {
         free(buf);
         return -1;
      }
   }
   else
   {
      size_t buf_cap = cap + (size_t)offset + 1;
      buf = malloc(buf_cap);
      char *err = malloc(4096), *quoted = shell_quote(abs);
      char command[MAX_PATH_LEN * 4 + 16];
      if (!buf || !err || !quoted ||
          snprintf(command, sizeof command, "cat %s", quoted) >= (int)sizeof command)
      {
         free(buf);
         free(err);
         free(quoted);
         return -1;
      }
      free(quoted);
      delegate_exec_result_t result = {0, 0, buf, buf_cap, err, 4096};
      int failed = docker_exec(self, state, command, 30000, &result) || result.exit_code;
      free(err);
      if (failed)
      {
         free(buf);
         return -1;
      }
      n = strlen(buf);
   }
   size_t start = (size_t)offset < n ? (size_t)offset : n;
   size_t take = n - start < cap ? n - start : cap;
   memmove(buf, buf + start, take);
   buf[take] = 0;
   *out = buf;
   return 0;
}

static int docker_write_file(delegate_backend_t *self, void *state, const char *p,
                             const char *content)
{
   if (!state || !p || !content)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;

   if (st->engine_fd >= 0)
   {
      char *bytes = NULL;
      size_t length = 0;
      int unknown = 0;
      int rc = docker_owned_file_request(st, abs, 'P', content, strlen(content), 30000, &bytes,
                                         &length, &unknown);
      free(bytes);
      return rc && unknown ? SCOPED_WRITE_OUTCOME_UNKNOWN : rc;
   }

   size_t clen = strlen(content);
   size_t b64_cap = ((clen + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
      return -1;
   if (util_b64_encode(content, clen, b64, b64_cap) < 0)
   {
      free(b64);
      return -1;
   }
   /* Quote the actual parent directly; command substitution loses newline names. */
   char *parent = strdup(abs), *quoted = shell_quote(abs), *parent_quoted = NULL;
   if (parent)
   {
      char *slash = strrchr(parent, '/');
      if (slash)
      {
         slash[slash == parent ? 1 : 0] = 0;
         parent_quoted = shell_quote(parent);
      }
   }
   free(parent);
   if (!quoted || !parent_quoted)
   {
      free(b64);
      free(quoted);
      free(parent_quoted);
      return -1;
   }
   size_t cmd_cap = strlen(quoted) + strlen(parent_quoted) + b64_cap + 128;
   char *cmd = malloc(cmd_cap);
   if (!cmd)
   {
      free(b64);
      free(quoted);
      free(parent_quoted);
      return -1;
   }
   snprintf(cmd, cmd_cap, "mkdir -p %s && printf '%%s' '%s' | base64 -d > %s", parent_quoted, b64,
            quoted);
   free(b64);
   free(quoted);
   free(parent_quoted);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   int rc = docker_exec(self, state, cmd, 30000, &r);
   free(cmd);
   if (rc != 0 || r.exit_code != 0)
      return -1;
   return 0;
}

static int docker_list_dir(delegate_backend_t *self, void *state, const char *p,
                           char ***entries_out)
{
   if (entries_out)
      *entries_out = NULL;
   if (!state || !p || !entries_out)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
   {
      /* The tool surfaces only "glob failed"; log why so a list_files that keeps failing
       * on a delegate's own worktree is diagnosable (path outside workdir, or ".."). */
      LOG_WARN("delegate-backend-docker", "list_dir: path '%s' did not resolve in workspace '%s'",
               p, st->workdir[0] ? st->workdir : resolve_docker_workdir());
      return -1;
   }

   if (st->engine_fd >= 0)
   {
      char *bytes = NULL;
      size_t length = 0;
      int unknown = 0;
      if (docker_owned_file_request(st, abs, 'D', NULL, 0, 30000, &bytes, &length, &unknown))
         return -1;
      size_t count = 0;
      for (size_t i = 0; i < length;)
      {
         char *end = memchr(bytes + i, 0, length - i);
         if (!end || end == bytes + i || ++count > 65536)
         {
            free(bytes);
            return -1;
         }
         i = (size_t)(end - bytes) + 1;
      }
      char **entries = calloc(count + 1, sizeof *entries);
      if (!entries)
      {
         free(bytes);
         return -1;
      }
      size_t offset = 0;
      for (size_t i = 0; i < count; i++)
      {
         entries[i] = strdup(bytes + offset);
         if (!entries[i])
         {
            for (size_t j = 0; j < i; j++)
               free(entries[j]);
            free(entries);
            free(bytes);
            return -1;
         }
         offset += strlen(bytes + offset) + 1;
      }
      free(bytes);
      *entries_out = entries;
      return (int)count;
   }

   char buf[64 * 1024] = {0};
   char err[4096] = {0};
   delegate_exec_result_t r = {0, 0, buf, sizeof(buf), err, sizeof(err)};
   char *quoted = shell_quote(abs);
   char cmd[MAX_PATH_LEN * 4 + 16];
   if (!quoted || snprintf(cmd, sizeof cmd, "ls -1A %s", quoted) >= (int)sizeof cmd)
   {
      free(quoted);
      return -1;
   }
   free(quoted);
   int xrc = docker_exec(self, state, cmd, 30000, &r);
   if (xrc != 0 || r.exit_code != 0)
   {
      LOG_WARN("delegate-backend-docker",
               "list_dir: 'ls -1A %s' failed (exec_rc=%d exit=%d): %.200s", abs, xrc, r.exit_code,
               err[0] ? err : "(no stderr)");
      return -1;
   }

   int n = 0;
   for (const char *q = buf; *q; q++)
      if (*q == '\n')
         n++;
   if (buf[0] && buf[strlen(buf) - 1] != '\n')
      n++;

   char **entries = calloc((size_t)n + 1, sizeof(*entries));
   if (!entries)
      return -1;
   int i = 0;
   const char *q = buf;
   while (*q && i < n)
   {
      const char *line_start = q;
      while (*q && *q != '\n')
         q++;
      size_t len = (size_t)(q - line_start);
      if (len > 0)
      {
         entries[i] = malloc(len + 1);
         if (!entries[i])
         {
            for (int j = 0; j < i; j++)
               free(entries[j]);
            free(entries);
            return -1;
         }
         memcpy(entries[i], line_start, len);
         entries[i][len] = '\0';
         i++;
      }
      if (*q == '\n')
         q++;
   }
   entries[i] = NULL;
   *entries_out = entries;
   return i;
}

static int docker_get_cwd(delegate_backend_t *self, void *state, char **out)
{
   (void)self;
   if (!state || !out)
      return -1;
   docker_state_t *st = state;
   *out = strdup(st->cwd[0] ? st->cwd : resolve_docker_workdir());
   return *out ? 0 : -1;
}

static int docker_set_cwd(delegate_backend_t *self, void *state, const char *path)
{
   (void)self;
   if (!state || !path || !path[0])
      return -1;
   docker_state_t *st = state;
   snprintf(st->cwd, sizeof(st->cwd), "%s", path);
   return 0;
}

static const delegate_owned_resources_t g_owned_resources = {
    .identity = delegate_backend_docker_owned_identity,
    .retire = delegate_backend_docker_owned_retire,
    .forwarder = delegate_backend_docker_owned_forwarder,
    .transport = delegate_backend_docker_owned_transport,
    .file_owner_start = delegate_backend_docker_file_owner_start,
    .prepared = delegate_backend_docker_owned_prepared,
    .file = delegate_backend_docker_owned_file,
};

static delegate_backend_t g_docker = {
    .owned_resources = &g_owned_resources,
    .name = "docker",
    .description = "long-lived container per task_id",
    .acquire = docker_acquire,
    .release = docker_release,
    .exec = docker_exec,
    .exec_scoped = docker_exec_scoped,
    .read_file = docker_read_file,
    .write_file = docker_write_file,
    .list_dir = docker_list_dir,
    .get_cwd = docker_get_cwd,
    .set_cwd = docker_set_cwd,
};

int delegate_backend_register_docker(void)
{
   return delegate_backend_register(&g_docker);
}

delegate_backend_t *delegate_backend_docker_get(void)
{
   return &g_docker;
}

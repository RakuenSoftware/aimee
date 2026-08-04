#define _GNU_SOURCE
#include <aimee/core/event_bus/bus_runtime.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_region_host.h>

#define BUS_RUNTIME_MAX_MODULES 64U
#define BUS_RUNTIME_MAX_EVENTS  64U
#define BUS_RUNTIME_IO_TIMEOUT  2

struct bus_runtime
{
   bus_host_t *host;
   pthread_mutex_t *host_lock;
   int listener_fd;
   pthread_t thread;
   atomic_int stop;
   char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   uint64_t stale_after_ns;
   uint64_t last_reap_ns;
   const bus_runtime_grant_t *grants;
   size_t grant_count;
   int pending_peer_pid;
   int slot_pidfd[BUS_ARENA_MAX_SLOTS];
};

typedef struct
{
   bus_runtime_grant_t grant;
   char executable[PATH_MAX];
   uint32_t publish[BUS_RUNTIME_MAX_EVENTS];
   uint32_t subscribe[BUS_RUNTIME_MAX_EVENTS];
   uint32_t request[BUS_RUNTIME_MAX_EVENTS];
   uint32_t serve[BUS_RUNTIME_MAX_EVENTS];
} owned_grant_t;

struct bus_runtime_policy
{
   owned_grant_t owned[BUS_RUNTIME_MAX_MODULES];
   bus_runtime_grant_t public[BUS_RUNTIME_MAX_MODULES];
   size_t count;
};

uint64_t bus_runtime_monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static const bus_runtime_grant_t *grant_find(const bus_runtime_t *runtime,
                                             const bus_attach_request_t *request)
{
   for (size_t i = 0; i < runtime->grant_count; i++)
      if (runtime->grants[i].principal_class == request->principal_class &&
          runtime->grants[i].principal_ref == request->principal_ref)
         return &runtime->grants[i];
   return NULL;
}

static int peer_identity(int fd, uint32_t *uid_out, int *pid_out, char *exe, size_t exe_size)
{
#if defined(SO_PEERCRED) && defined(__linux__)
   struct ucred peer;
   socklen_t peer_size = sizeof(peer);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
       peer_size != sizeof(peer) || peer.pid <= 0)
      return -1;
   char proc[64];
   int n = snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)peer.pid);
   if (n <= 0 || (size_t)n >= sizeof(proc))
      return -1;
   ssize_t got = readlink(proc, exe, exe_size - 1U);
   if (got <= 0 || (size_t)got >= exe_size)
      return -1;
   exe[got] = '\0';
   *uid_out = (uint32_t)peer.uid;
   *pid_out = (int)peer.pid;
   return 0;
#else
   (void)fd;
   (void)uid_out;
   (void)pid_out;
   (void)exe;
   (void)exe_size;
   errno = ENOTSUP;
   return -1;
#endif
}

static bus_attach_status_t runtime_admit(void *ctx, int fd, const bus_attach_request_t *request)
{
   bus_runtime_t *runtime = ctx;
   runtime->pending_peer_pid = 0;
   const bus_runtime_grant_t *grant = grant_find(runtime, request);
   if (!grant || !grant->executable || grant->executable[0] != '/')
      return BUS_ATTACH_DENIED_POLICY;
   uint32_t peer_uid = 0;
   int peer_pid = 0;
   char peer_exe[PATH_MAX];
   if (peer_identity(fd, &peer_uid, &peer_pid, peer_exe, sizeof(peer_exe)) != 0)
      return BUS_ATTACH_DENIED_POLICY;
   uint32_t expected_uid = grant->uid == BUS_RUNTIME_SELF_UID ? (uint32_t)geteuid() : grant->uid;
   if (peer_uid != expected_uid || strcmp(peer_exe, grant->executable) != 0)
      return BUS_ATTACH_DENIED_POLICY;
   runtime->pending_peer_pid = peer_pid;
   return BUS_ATTACH_OK;
}

static int process_handle_open(int pid)
{
#if defined(__linux__) && defined(SYS_pidfd_open)
   return (int)syscall(SYS_pidfd_open, pid, 0);
#else
   (void)pid;
   errno = ENOTSUP;
   return -1;
#endif
}

static int process_handle_exited(int fd)
{
   if (fd < 0)
      return 0;
   struct pollfd process = {.fd = fd, .events = POLLIN};
   int rc;
   do
      rc = poll(&process, 1, 0);
   while (rc < 0 && errno == EINTR);
   return rc == 1 && (process.revents & (POLLIN | POLLHUP | POLLERR));
}

static void process_handle_close(int *fd)
{
   if (*fd >= 0)
      close(*fd);
   *fd = -1;
}

static int grant_many(bus_host_t *host, uint32_t slot, const uint32_t *kinds, size_t count,
                      uint8_t patterns)
{
   for (size_t i = 0; i < count; i++)
      if (bus_host_grant_outbound(host, slot, kinds[i], patterns) != BUS_HOST_OK)
         return -1;
   return 0;
}

static bus_attach_status_t runtime_bind(void *ctx, bus_host_t *host, uint32_t slot,
                                        const bus_attach_request_t *request)
{
   bus_runtime_t *runtime = ctx;
   const bus_runtime_grant_t *grant = grant_find(runtime, request);
   int peer_pidfd = process_handle_open(runtime->pending_peer_pid);
   runtime->pending_peer_pid = 0;
   /* A crashed process cannot detach its shared-memory slot. Prefer an exact
    * kernel process handle over the heartbeat timeout when its supervisor
    * immediately launches the same principal again. A live duplicate is still
    * denied, and kernels without pidfd support retain heartbeat-only reaping. */
   for (uint32_t i = 0; grant && i < host->cfg.max_slots; ++i)
   {
      if (i == slot || !host->slots[i].in_use ||
          host->slots[i].principal_class != request->principal_class ||
          host->slots[i].principal_ref != request->principal_ref)
         continue;
      if (!process_handle_exited(runtime->slot_pidfd[i]))
         goto denied;
      process_handle_close(&runtime->slot_pidfd[i]);
      if (bus_host_release_slot(host, i) != BUS_HOST_OK)
         goto denied;
   }
   if (!grant || bus_host_enforce_grants(host, slot) != BUS_HOST_OK ||
       grant_many(host, slot, grant->publish, grant->publish_count, BUS_GRANT_NOTIFY) != 0 ||
       grant_many(host, slot, grant->request, grant->request_count, BUS_GRANT_REQUEST) != 0)
      goto denied;
   for (size_t i = 0; i < grant->subscribe_count; i++)
      if (bus_host_subscribe(host, slot, grant->subscribe[i]) != BUS_HOST_OK)
         goto denied;
   for (size_t i = 0; i < grant->serve_count; i++)
      if (bus_host_serve_kind(host, slot, grant->serve[i]) != BUS_HOST_OK)
         goto denied;
   process_handle_close(&runtime->slot_pidfd[slot]);
   runtime->slot_pidfd[slot] = peer_pidfd;
   return BUS_ATTACH_OK;

denied:
   process_handle_close(&peer_pidfd);
   return BUS_ATTACH_DENIED_POLICY;
}

static void connection_timeouts(int fd)
{
   struct timeval timeout = {.tv_sec = BUS_RUNTIME_IO_TIMEOUT, .tv_usec = 0};
   (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static void *accept_main(void *arg)
{
   bus_runtime_t *runtime = arg;
   const struct timespec idle = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
   while (!atomic_load_explicit(&runtime->stop, memory_order_acquire))
   {
      int fd = -1;
      if (bus_endpoint_accept(runtime->listener_fd, &fd) != 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
         {
            nanosleep(&idle, NULL);
            continue;
         }
         if (!atomic_load_explicit(&runtime->stop, memory_order_acquire))
            nanosleep(&idle, NULL);
         continue;
      }
      connection_timeouts(fd);
      pthread_mutex_lock(runtime->host_lock);
      (void)bus_host_serve_attach(runtime->host, fd);
      pthread_mutex_unlock(runtime->host_lock);
      (void)bus_endpoint_close(&fd);
   }
   return NULL;
}

static int listen_fresh(const char *path, unsigned mode, int backlog, int *fd_out)
{
   if (bus_endpoint_listen(path, mode, backlog, fd_out) == 0)
      return 0;
   if (errno != EADDRINUSE)
      return -1;
   int probe = -1;
   if (bus_endpoint_connect(path, &probe) == 0)
   {
      bus_endpoint_close(&probe);
      errno = EADDRINUSE; /* a live owner keeps the endpoint */
      return -1;
   }
   if (errno != ECONNREFUSED && errno != ENOENT)
      return -1;
   if (bus_endpoint_remove(path) != 0)
      return -1;
   return bus_endpoint_listen(path, mode, backlog, fd_out);
}

bus_runtime_t *bus_runtime_start(bus_host_t *host, pthread_mutex_t *host_lock,
                                 const bus_runtime_config_t *config)
{
   if (!host || !host_lock || !config || !config->socket_path || config->socket_path[0] != '/' ||
       config->socket_mode > 0777U || config->backlog <= 0 ||
       (config->grant_count > 0 && !config->grants))
   {
      errno = EINVAL;
      return NULL;
   }
   bus_runtime_t *runtime = calloc(1, sizeof(*runtime));
   if (!runtime)
      return NULL;
   runtime->listener_fd = -1;
   for (uint32_t i = 0; i < BUS_ARENA_MAX_SLOTS; ++i)
      runtime->slot_pidfd[i] = -1;
   runtime->host = host;
   runtime->host_lock = host_lock;
   runtime->stale_after_ns = config->stale_after_ns;
   runtime->grants = config->grants;
   runtime->grant_count = config->grant_count;
   if (snprintf(runtime->socket_path, sizeof(runtime->socket_path), "%s", config->socket_path) <=
           0 ||
       strlen(config->socket_path) >= sizeof(runtime->socket_path))
   {
      errno = ENAMETOOLONG;
      free(runtime);
      return NULL;
   }
   if (listen_fresh(runtime->socket_path, config->socket_mode, config->backlog,
                    &runtime->listener_fd) != 0)
   {
      free(runtime);
      return NULL;
   }
   int flags = fcntl(runtime->listener_fd, F_GETFL);
   if (flags < 0 || fcntl(runtime->listener_fd, F_SETFL, flags | O_NONBLOCK) != 0)
      goto fail;
   bus_host_set_admission(host, runtime_admit, runtime);
   bus_host_set_attach_hook(host, runtime_bind, runtime);
   if (pthread_create(&runtime->thread, NULL, accept_main, runtime) != 0)
      goto fail;
   return runtime;

fail:
   bus_host_set_admission(host, NULL, NULL);
   bus_host_set_attach_hook(host, NULL, NULL);
   bus_endpoint_close(&runtime->listener_fd);
   (void)bus_endpoint_remove(runtime->socket_path);
   free(runtime);
   return NULL;
}

void bus_runtime_stop(bus_runtime_t **runtime_ptr)
{
   if (!runtime_ptr || !*runtime_ptr)
      return;
   bus_runtime_t *runtime = *runtime_ptr;
   atomic_store_explicit(&runtime->stop, 1, memory_order_release);
   /* The listener is nonblocking, so the accept loop observes stop within one
    * short idle tick. Join before writing listener_fd to avoid racing the
    * accept thread's read of that field. */
   pthread_join(runtime->thread, NULL);
   bus_endpoint_close(&runtime->listener_fd);
   pthread_mutex_lock(runtime->host_lock);
   bus_host_set_admission(runtime->host, NULL, NULL);
   bus_host_set_attach_hook(runtime->host, NULL, NULL);
   for (uint32_t i = 0; i < BUS_ARENA_MAX_SLOTS; ++i)
      process_handle_close(&runtime->slot_pidfd[i]);
   pthread_mutex_unlock(runtime->host_lock);
   (void)bus_endpoint_remove(runtime->socket_path);
   free(runtime);
   *runtime_ptr = NULL;
}

uint32_t bus_runtime_maintain(bus_runtime_t *runtime, uint64_t now_ns)
{
   if (!runtime || now_ns == 0)
      return 0;
   bus_control_heartbeat(runtime->host->control, now_ns);
   if (runtime->stale_after_ns == 0 ||
       now_ns - runtime->last_reap_ns < runtime->stale_after_ns / 4U)
      return 0;
   runtime->last_reap_ns = now_ns;
   uint32_t reaped = bus_host_reap(runtime->host, now_ns, runtime->stale_after_ns);
   for (uint32_t i = 0; i < runtime->host->cfg.max_slots; ++i)
      if (!runtime->host->slots[i].in_use)
         process_handle_close(&runtime->slot_pidfd[i]);
   return reaped;
}

static char *trim(char *value)
{
   while (*value == ' ' || *value == '\t')
      value++;
   char *end = value + strlen(value);
   while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
      *--end = '\0';
   return value;
}

static int parse_u32(const char *text, uint32_t *out)
{
   char *end = NULL;
   errno = 0;
   unsigned long value = strtoul(text, &end, 10);
   if (errno || !end || *end || end == text || value > UINT32_MAX)
      return -1;
   *out = (uint32_t)value;
   return 0;
}

static int parse_kinds(char *text, uint32_t *out, size_t *count)
{
   *count = 0;
   if (!text[0])
      return 0;
   char *save = NULL;
   for (char *one = strtok_r(text, ",", &save); one; one = strtok_r(NULL, ",", &save))
   {
      if (*count >= BUS_RUNTIME_MAX_EVENTS || parse_u32(trim(one), &out[*count]) != 0 ||
          out[*count] < BUS_KIND_MODULE_BASE)
         return -1;
      (*count)++;
   }
   return 0;
}

static int parse_grant_file(const char *path, owned_grant_t *owned)
{
   FILE *file = fopen(path, "r");
   if (!file)
      return -1;
   memset(owned, 0, sizeof(*owned));
   owned->grant.uid = BUS_RUNTIME_SELF_UID;
   char *line = NULL;
   size_t cap = 0;
   int have_version = 0, have_class = 0, have_ref = 0, have_exe = 0, ok = 1;
   while (ok && getline(&line, &cap, file) >= 0)
   {
      char *current = trim(line);
      if (!current[0] || current[0] == '#')
         continue;
      char *equal = strchr(current, '=');
      if (!equal)
      {
         ok = 0;
         break;
      }
      *equal++ = '\0';
      char *key = trim(current), *value = trim(equal);
      if (strcmp(key, "version") == 0)
         have_version = strcmp(value, "1") == 0;
      else if (strcmp(key, "principal_class") == 0)
         have_class = parse_u32(value, &owned->grant.principal_class) == 0;
      else if (strcmp(key, "principal_ref") == 0)
         have_ref = parse_u32(value, &owned->grant.principal_ref) == 0;
      else if (strcmp(key, "uid") == 0)
         ok = strcmp(value, "self") == 0 ? (owned->grant.uid = BUS_RUNTIME_SELF_UID, 1)
                                         : parse_u32(value, &owned->grant.uid) == 0;
      else if (strcmp(key, "executable") == 0)
      {
         char resolved[PATH_MAX];
         int copied = value[0] == '/' && realpath(value, resolved) != NULL
                          ? snprintf(owned->executable, sizeof(owned->executable), "%s", resolved)
                          : -1;
         have_exe = copied > 0 && (size_t)copied < sizeof(owned->executable);
      }
      else if (strcmp(key, "publish") == 0)
         ok = parse_kinds(value, owned->publish, &owned->grant.publish_count) == 0;
      else if (strcmp(key, "subscribe") == 0)
         ok = parse_kinds(value, owned->subscribe, &owned->grant.subscribe_count) == 0;
      else if (strcmp(key, "request") == 0)
         ok = parse_kinds(value, owned->request, &owned->grant.request_count) == 0;
      else if (strcmp(key, "serve") == 0)
         ok = parse_kinds(value, owned->serve, &owned->grant.serve_count) == 0;
      else
         ok = 0;
   }
   free(line);
   if (ferror(file))
      ok = 0;
   fclose(file);
   if (!ok || !have_version || !have_class || !have_ref || !have_exe ||
       owned->grant.principal_class == 0 || owned->grant.principal_ref == 0)
      return -1;
   owned->grant.executable = owned->executable;
   owned->grant.publish = owned->publish;
   owned->grant.subscribe = owned->subscribe;
   owned->grant.request = owned->request;
   owned->grant.serve = owned->serve;
   return 0;
}

static int grant_name(const struct dirent *entry)
{
   size_t length = strlen(entry->d_name);
   return length > 6U && strcmp(entry->d_name + length - 6U, ".grant") == 0;
}

int bus_runtime_policy_load_dir(const char *directory, bus_runtime_policy_t **out)
{
   if (out)
      *out = NULL;
   if (!directory || directory[0] != '/' || !out)
   {
      errno = EINVAL;
      return -1;
   }
   bus_runtime_policy_t *policy = calloc(1, sizeof(*policy));
   if (!policy)
      return -1;
   DIR *dir = opendir(directory);
   if (!dir)
   {
      if (errno == ENOENT)
      {
         *out = policy;
         return 0;
      }
      free(policy);
      return -1;
   }
   int ok = 1;
   struct dirent *entry;
   while (ok && (entry = readdir(dir)) != NULL)
   {
      if (!grant_name(entry))
         continue;
      if (policy->count >= BUS_RUNTIME_MAX_MODULES)
      {
         ok = 0;
         break;
      }
      char path[PATH_MAX];
      int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
      if (n <= 0 || (size_t)n >= sizeof(path) ||
          parse_grant_file(path, &policy->owned[policy->count]) != 0)
      {
         ok = 0;
         break;
      }
      const bus_runtime_grant_t *candidate = &policy->owned[policy->count].grant;
      for (size_t i = 0; i < policy->count; i++)
         if (policy->public[i].principal_class == candidate->principal_class &&
             policy->public[i].principal_ref == candidate->principal_ref)
            ok = 0;
      if (ok)
         policy->public[policy->count++] = *candidate;
   }
   closedir(dir);
   if (!ok)
   {
      free(policy);
      errno = EINVAL;
      return -1;
   }
   *out = policy;
   return 0;
}

void bus_runtime_policy_free(bus_runtime_policy_t **policy)
{
   if (!policy)
      return;
   free(*policy);
   *policy = NULL;
}

const bus_runtime_grant_t *bus_runtime_policy_grants(const bus_runtime_policy_t *policy,
                                                     size_t *count_out)
{
   if (count_out)
      *count_out = policy ? policy->count : 0;
   return policy ? policy->public : NULL;
}

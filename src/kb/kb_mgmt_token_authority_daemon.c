#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_mgmt_token_authority_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

int kb_mgmt_token_authority_ipc_read_request(int fd, uint32_t timeout_ms, char correlation_id[65],
                                             char jti[65]);
int kb_mgmt_token_authority_ipc_write_response(int fd, uint32_t timeout_ms,
                                               kb_mgmt_token_authority_ipc_result_t status,
                                               const kb_mgmt_token_authority_output_t *out);
int kb_mgmt_token_authority_ipc_socket_matches(int fd, const char *path, gid_t gid, mode_t mode);

static volatile sig_atomic_t g_stop_requested;
static int g_hardened_listen_fd = -1;
static uid_t g_hardened_authority_uid;

static int preserved(int fd, const int *fds, size_t count)
{
   for (size_t i = 0; i < count; ++i)
      if (fds[i] == fd)
         return 1;
   return 0;
}

static int cleanse_fds(const int *fds, size_t count)
{
   if (!fds || !count)
      return -1;
   for (size_t i = 0; i < count; ++i)
      if (fds[i] < 0 || (i && preserved(fds[i], fds, i)))
         return -1;
   struct rlimit limit;
   if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
      return -1;
   rlim_t max = limit.rlim_cur;
   if (max == RLIM_INFINITY)
   {
      long open_max = sysconf(_SC_OPEN_MAX);
      if (open_max <= 0)
         return -1;
      max = (rlim_t)open_max;
   }
   /* Bounded by the process's existing descriptor ceiling. This is performed
    * once, before accepting secrets, and closes stdio unless explicitly kept. */
   for (rlim_t fd = 0; fd < max; ++fd)
      if (fd <= INT32_MAX && !preserved((int)fd, fds, count) && close((int)fd) != 0 &&
          errno != EBADF)
         return -1;
   return 0;
}

static int root_created_listener(int fd)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   struct ucred creator;
   socklen_t creator_len = sizeof(creator);
   return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creator, &creator_len) == 0 &&
          creator_len == sizeof(creator) && creator.uid == 0;
#else
   (void)fd;
   return 0;
#endif
}

static int valid_config(const kb_mgmt_token_authority_daemon_config_t *config)
{
   int accepting = 0;
   socklen_t accepting_len = sizeof(accepting);
   return config && config->listen_fd >= 0 && config->issue && config->socket_path &&
          strcmp(config->socket_path, KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH) == 0 &&
          config->authority_uid != 0 && config->kb_uid != 0 &&
          config->kb_uid != config->authority_uid && config->socket_mode == 0660 &&
          config->timeout_ms && config->timeout_ms <= 60000 &&
          preserved(config->listen_fd, config->preserve_fds, config->preserve_fd_count) &&
          getsockopt(config->listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_len) ==
              0 &&
          accepting_len == sizeof(accepting) && accepting &&
          root_created_listener(config->listen_fd) &&
          kb_mgmt_token_authority_ipc_socket_matches(config->listen_fd, config->socket_path,
                                                     config->socket_gid, config->socket_mode) != 0;
}

int kb_mgmt_token_authority_daemon_harden(const kb_mgmt_token_authority_daemon_config_t *config)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   struct rlimit no_core = {0, 0};
   if (g_hardened_listen_fd >= 0 || !valid_config(config) || getuid() != config->authority_uid ||
       geteuid() != config->authority_uid || config->authority_uid == 0 ||
       config->authority_uid == config->kb_uid || setrlimit(RLIMIT_CORE, &no_core) != 0 ||
       prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
      return -1;
#ifdef PR_GET_DUMPABLE
   if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
      return -1;
#endif
   int flags = fcntl(config->listen_fd, F_GETFL);
   int fd_flags = fcntl(config->listen_fd, F_GETFD);
   if (flags < 0 || fd_flags < 0 || fcntl(config->listen_fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
       fcntl(config->listen_fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
      return -1;
   (void)umask(077);
   if (cleanse_fds(config->preserve_fds, config->preserve_fd_count) != 0)
      return -1;
   g_hardened_listen_fd = config->listen_fd;
   g_hardened_authority_uid = config->authority_uid;
   return 0;
#else
   (void)config;
   errno = ENOTSUP; /* Refuse platforms without Linux peer/process hardening. */
   return -1;
#endif
}

void kb_mgmt_token_authority_daemon_request_stop(void)
{
   g_stop_requested = 1;
}

static int peer_is_kb(int fd, uid_t expected)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   struct ucred peer;
   socklen_t peer_len = sizeof(peer);
   return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) == 0 &&
          peer_len == sizeof(peer) && peer.uid == expected;
#else
   (void)fd;
   (void)expected;
   return 0;
#endif
}

static void serve_one(const kb_mgmt_token_authority_daemon_config_t *config, int fd)
{
   char correlation_id[65] = "";
   char jti[65] = "";
   kb_mgmt_token_authority_output_t output;
   memset(&output, 0, sizeof(output));
   if (!peer_is_kb(fd, config->kb_uid))
      return; /* Do not parse or answer an unauthorized peer. */
   kb_mgmt_token_authority_ipc_result_t result = KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID;
   if (kb_mgmt_token_authority_ipc_read_request(fd, config->timeout_ms, correlation_id, jti) == 0)
   {
      result = config->issue(correlation_id, jti, &output, config->issue_opaque);
      if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
         OPENSSL_cleanse(&output, sizeof(output));
   }
   (void)kb_mgmt_token_authority_ipc_write_response(fd, config->timeout_ms, result, &output);
   OPENSSL_cleanse(&output, sizeof(output));
   OPENSSL_cleanse(correlation_id, sizeof(correlation_id));
   OPENSSL_cleanse(jti, sizeof(jti));
}

int kb_mgmt_token_authority_daemon_run(const kb_mgmt_token_authority_daemon_config_t *config)
{
   if (!valid_config(config) || g_hardened_listen_fd != config->listen_fd ||
       g_hardened_authority_uid != config->authority_uid || getuid() != config->authority_uid ||
       geteuid() != config->authority_uid)
      return -1;

   while (!g_stop_requested)
   {
      struct pollfd ready = {.fd = config->listen_fd, .events = POLLIN};
      int poll_rc = poll(&ready, 1, 250);
      if (g_stop_requested)
         break;
      if (poll_rc < 0 && errno == EINTR)
         continue;
      if (poll_rc < 0)
         return -1;
      if (poll_rc == 0)
         continue;
      if (!(ready.revents & POLLIN) || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
         return -1;
      int fd = accept4(config->listen_fd, NULL, NULL, SOCK_CLOEXEC);
      if (fd < 0)
      {
         if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
         return -1;
      }
      /* Deliberately sequential: no worker, fork, queue, or second in-flight
       * authority operation exists in this process. */
      serve_one(config, fd);
      (void)shutdown(fd, SHUT_RDWR);
      (void)close(fd);
   }
   return 0;
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_mgmt_token_authority_ipc.h"

#include <assert.h>
#ifdef NDEBUG
#error "P5-C2d regression tests require active assertions"
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static const uid_t authority_uid = 1002;
static int closed_fds[8];
static int core_disabled;
static int dump_disabled;
static int no_new_privs;
static int accepted;
static int polled;
static int read_requests;
static int issue_calls;
static int responses;
static volatile sig_atomic_t saw_sigterm;

static uid_t test_getuid(void)
{
   return authority_uid;
}

static uid_t test_geteuid(void)
{
   return authority_uid;
}

static int test_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
   assert(level == SOL_SOCKET);
   if (option == SO_ACCEPTCONN)
   {
      assert(*length >= sizeof(int));
      *(int *)value = 1;
      *length = sizeof(int);
      return 0;
   }
   assert(option == SO_PEERCRED && *length >= sizeof(struct ucred));
   struct ucred *creator = value;
   memset(creator, 0, sizeof(*creator));
   creator->uid = fd == 3 ? 0 : 1001;
   *length = sizeof(*creator);
   return 0;
}

static int test_getrlimit(int resource, struct rlimit *limit)
{
   assert(resource == RLIMIT_NOFILE);
   limit->rlim_cur = 8;
   limit->rlim_max = 8;
   return 0;
}

static int test_setrlimit(int resource, const struct rlimit *limit)
{
   assert(resource == RLIMIT_CORE && limit->rlim_cur == 0 && limit->rlim_max == 0);
   core_disabled = 1;
   return 0;
}

static int test_prctl(int option, ...)
{
   if (option == PR_SET_DUMPABLE)
      dump_disabled = 1;
   else if (option == PR_SET_NO_NEW_PRIVS)
      no_new_privs = 1;
   else if (option == PR_GET_DUMPABLE)
      return dump_disabled ? 0 : 1;
   else
      assert(0);
   return 0;
}

static int test_fcntl(int fd, int command, ...)
{
   assert(fd == 3);
   if (command == F_GETFL || command == F_GETFD)
      return 0;
   assert(command == F_SETFL || command == F_SETFD);
   return 0;
}

static int test_close(int fd)
{
   if (fd >= 0 && fd < 8)
      closed_fds[fd] = 1;
   return 0;
}

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
   assert(count == 1 && timeout == 250);
   assert(!polled && fds[0].fd == 3 && fds[0].events == POLLIN);
   polled = 1;
   fds[0].revents = POLLIN;
   return 1;
}

static int test_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
   (void)fd;
   (void)address;
   (void)length;
   assert(fd == 3 && flags == SOCK_CLOEXEC && !accepted);
   accepted = 1;
   return 4;
}

#define accept4    test_accept4
#define close      test_close
#define fcntl      test_fcntl
#define geteuid    test_geteuid
#define getrlimit  test_getrlimit
#define getsockopt test_getsockopt
#define getuid     test_getuid
#define poll       test_poll
#define prctl      test_prctl
#define setrlimit  test_setrlimit
#include "../kb/kb_mgmt_token_authority_daemon.c"
#undef accept4
#undef close
#undef fcntl
#undef geteuid
#undef getrlimit
#undef getsockopt
#undef getuid
#undef poll
#undef prctl
#undef setrlimit

int kb_mgmt_token_authority_ipc_read_request(int fd, uint32_t timeout_ms, char correlation_id[65],
                                             char jti[65])
{
   assert(fd == 4 && timeout_ms == 1000 && !read_requests);
   ++read_requests;
   memset(correlation_id, 'a', 64);
   correlation_id[64] = 0;
   memset(jti, 'b', 64);
   jti[64] = 0;
   return 0;
}

int kb_mgmt_token_authority_ipc_write_response(int fd, uint32_t timeout_ms,
                                               kb_mgmt_token_authority_ipc_result_t status,
                                               const kb_mgmt_token_authority_output_t *out)
{
   assert(fd == 4 && timeout_ms == 1000 && status == KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE &&
          out && out->jwt_len == 0 && out->jwt[0] == 0 && !responses);
   ++responses;
   return 0;
}

int kb_mgmt_token_authority_ipc_socket_matches(int fd, const char *path, gid_t gid, mode_t mode)
{
   (void)fd;
   (void)path;
   (void)gid;
   (void)mode;
   return 1;
}

static kb_mgmt_token_authority_ipc_result_t issue(const char *correlation_id, const char *jti,
                                                  kb_mgmt_token_authority_output_t *out,
                                                  void *opaque)
{
   assert(correlation_id && correlation_id[0] == 'a' && strlen(correlation_id) == 64);
   assert(jti && jti[0] == 'b' && strlen(jti) == 64);
   assert(out && out->jwt_len == 0 && !issue_calls);
   (void)opaque;
   ++issue_calls;
   /* SIGTERM is delivered synchronously to this daemon/issue thread. The
    * handler sets only sig_atomic state; serve_one must finish its response,
    * after which daemon_run observes the stop and does not accept again. */
   assert(raise(SIGTERM) == 0);
   return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
}

static void stop_handler(int signal_number)
{
   saw_sigterm = signal_number == SIGTERM;
   kb_mgmt_token_authority_daemon_request_stop();
}

int main(void)
{
   static const int preserved[] = {3, 5};
   kb_mgmt_token_authority_daemon_config_t config = {
       .listen_fd = 3,
       .socket_path = KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH,
       .authority_uid = authority_uid,
       .kb_uid = 1001,
       .socket_gid = 1003,
       .socket_mode = 0660,
       .timeout_ms = 1000,
       .preserve_fds = preserved,
       .preserve_fd_count = 2,
       .issue = issue,
   };

   assert(kb_mgmt_token_authority_daemon_run(&config) == -1); /* no pre-custody hardening */
   assert(kb_mgmt_token_authority_daemon_harden(&config) == 0);
   assert(core_disabled && dump_disabled && no_new_privs);
   for (int fd = 0; fd < 8; ++fd)
      assert(closed_fds[fd] == (fd != 3 && fd != 5));

   struct sigaction action;
   memset(&action, 0, sizeof(action));
   action.sa_handler = stop_handler;
   sigemptyset(&action.sa_mask);
   assert(sigaction(SIGTERM, &action, NULL) == 0);
   assert(kb_mgmt_token_authority_daemon_run(&config) == 0);
   assert(polled == 1 && accepted == 1 && read_requests == 1 && issue_calls == 1 &&
          responses == 1 && saw_sigterm);
   return 0;
}

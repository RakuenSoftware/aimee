#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_mgmt_token_authority_ipc.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Compile the real client into this test translation unit with only the
 * namespace/credential syscalls replaced. Data still crosses a real Unix
 * socketpair, including shutdown(SHUT_WR), EOF and partial-response behavior. */
static int test_client_fd = -1;
static int test_mode;
static int test_send_calls;
static const uid_t kb_uid = 1001;
/* The activated listener is accepted as the root peer that created it. */
static const uid_t authority_uid = 0;
static const gid_t socket_gid = 1003;

static uid_t test_getuid(void)
{
   return kb_uid;
}

static uid_t test_geteuid(void)
{
   return kb_uid;
}

static int test_lstat(const char *path, struct stat *st)
{
   memset(st, 0, sizeof(*st));
   st->st_uid = 0;
   if (!strcmp(path, KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH))
   {
      st->st_gid = socket_gid;
      st->st_mode = S_IFSOCK | 0660;
   }
   else
      st->st_mode = S_IFDIR | 0755;
   return 0;
}

static int test_socket(int domain, int type, int protocol)
{
   (void)domain;
   (void)type;
   (void)protocol;
   int fd = test_client_fd;
   test_client_fd = -1;
   return fd;
}

static int test_connect(int fd, const struct sockaddr *address, socklen_t length)
{
   (void)fd;
   (void)address;
   (void)length;
   return 0;
}

static int test_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
   (void)fd;
   assert(level == SOL_SOCKET && option == SO_PEERCRED && *length >= sizeof(struct ucred));
   struct ucred *peer = value;
   memset(peer, 0, sizeof(*peer));
   peer->uid = authority_uid;
   *length = sizeof(*peer);
   return 0;
}

static ssize_t test_send(int fd, const void *data, size_t length, int flags)
{
   (void)flags;
   if (test_mode == 5 || (test_mode == 4 && test_send_calls++ > 0))
   {
      errno = EPIPE;
      return -1;
   }
   if (test_mode == 4 && length > 17)
      length = 17;
   return write(fd, data, length);
}

#define getuid     test_getuid
#define geteuid    test_geteuid
#define lstat      test_lstat
#define send       test_send
#define socket     test_socket
#define connect    test_connect
#define getsockopt test_getsockopt
#include "../kb/kb_mgmt_token_authority_ipc.c"
#undef connect
#undef geteuid
#undef getsockopt
#undef getuid
#undef lstat
#undef send
#undef socket

static void *authority_peer(void *opaque)
{
   int fd = *(int *)opaque;
   unsigned char request[12 + 64 + 64];
   size_t used = 0;
   if (test_mode == 4 || test_mode == 5)
   {
      ssize_t n;
      while ((n = read(fd, request + used, sizeof(request) - used)) > 0)
         used += (size_t)n;
      assert(n == 0);
      assert(used == (test_mode == 4 ? 17u : 0u));
      close(fd);
      return NULL;
   }
   while (used < sizeof(request))
   {
      ssize_t n = read(fd, request + used, sizeof(request) - used);
      assert(n > 0);
      used += (size_t)n;
   }
   unsigned char extra = 0;
   assert(read(fd, &extra, 1) == 0); /* proves the complete request was sent */
   assert(!memcmp(request, "AMTQ\1\1", 6));
   if (test_mode == 1)
   {
      static const unsigned char partial[] = {'A', 'M', 'T', 'R', 1, 1};
      assert(write(fd, partial, sizeof(partial)) == (ssize_t)sizeof(partial));
   }
   else if (test_mode == 2)
   {
      static const unsigned char denied[] = {
          'A', 'M', 'T', 'R', 1, 1, 0, KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED, 0, 0, 0, 0};
      assert(write(fd, denied, sizeof(denied)) == (ssize_t)sizeof(denied));
   }
   else if (test_mode == 3)
   {
      static const unsigned char malformed[] = {
          'X', 'M', 'T', 'R', 1, 1, 0, KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED, 0, 0, 0, 0};
      assert(write(fd, malformed, sizeof(malformed)) == (ssize_t)sizeof(malformed));
   }
   close(fd);
   return NULL;
}

static kb_mgmt_token_authority_ipc_result_t run_case(int mode,
                                                     kb_mgmt_token_authority_output_t *out)
{
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   test_client_fd = pair[0];
   test_mode = mode;
   test_send_calls = 0;
   pthread_t thread;
   assert(pthread_create(&thread, NULL, authority_peer, &pair[1]) == 0);
   kb_mgmt_token_authority_client_config_t config = {
       .socket_path = KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH,
       .socket_gid = socket_gid,
       .socket_mode = 0660,
       .timeout_ms = 1000,
   };
   char correlation[65] = {0}, jti[65] = {0};
   memset(correlation, 'a', 64);
   memset(jti, 'b', 64);
   kb_mgmt_token_authority_ipc_result_t result =
       kb_mgmt_token_authority_client_issue(&config, correlation, jti, out);
   assert(pthread_join(thread, NULL) == 0);
   assert(test_client_fd == -1);
   return result;
}

int main(void)
{
   kb_mgmt_token_authority_output_t out;
   memset(&out, 0x5a, sizeof(out));
   assert(run_case(0, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);

   memset(&out, 0x5a, sizeof(out));
   assert(run_case(1, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);

   memset(&out, 0x5a, sizeof(out));
   assert(run_case(2, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);

   memset(&out, 0x5a, sizeof(out));
   assert(run_case(3, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);

   memset(&out, 0x5a, sizeof(out));
   assert(run_case(4, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);

   memset(&out, 0x5a, sizeof(out));
   assert(run_case(5, &out) == KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE);
   assert(out.jwt_len == 0 && out.jwt[0] == 0);
   return 0;
}

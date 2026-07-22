#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_mgmt_token_authority_ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_HEADER_LEN  12u
#define RESPONSE_HEADER_LEN 12u

static const unsigned char request_magic[4] = {'A', 'M', 'T', 'Q'};
static const unsigned char response_magic[4] = {'A', 'M', 'T', 'R'};

static void clear_output(kb_mgmt_token_authority_output_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
}

static int valid_path(const char *path)
{
   if (!path || path[0] != '/' || path[1] == '\0')
      return 0;
   size_t n = 0;
   while (n < sizeof(((struct sockaddr_un *)0)->sun_path) && path[n])
      ++n;
   return n > 1 && n < sizeof(((struct sockaddr_un *)0)->sun_path);
}

static int lower_hex_exact(const char *text, size_t exact)
{
   if (!text || strnlen(text, exact + 1) != exact)
      return 0;
   for (size_t i = 0; i < exact; ++i)
      if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
         return 0;
   return 1;
}

static int valid_jwt(const char *jwt, size_t len)
{
   unsigned dots = 0;
   if (!jwt || len < 5 || len > KB_MGMT_TOKEN_WIRE_MAX)
      return 0;
   for (size_t i = 0; i < len; ++i)
   {
      unsigned char c = (unsigned char)jwt[i];
      if (c == '.')
         ++dots;
      else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-'))
         return 0;
   }
   return dots == 2;
}

static int64_t monotonic_ms(void)
{
   struct timespec now;
   return clock_gettime(CLOCK_MONOTONIC, &now) == 0
              ? (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000
              : -1;
}

static int wait_for(int fd, short events, int64_t deadline)
{
   int64_t now = monotonic_ms();
   if (now < 0 || now >= deadline)
      return -1;
   int64_t left = deadline - now;
   struct pollfd pfd = {.fd = fd, .events = events};
   int rc;
   do
      rc = poll(&pfd, 1, left > INT32_MAX ? INT32_MAX : (int)left);
   while (rc < 0 && errno == EINTR);
   int ready = (pfd.revents & events) || ((events & POLLIN) && (pfd.revents & POLLHUP));
   return rc == 1 && ready && !(pfd.revents & (POLLERR | POLLNVAL)) ? 0 : -1;
}

static int write_all_count(int fd, const unsigned char *data, size_t len, int64_t deadline,
                           size_t *written)
{
   size_t done = 0;
   if (written)
      *written = 0;
   while (done < len)
   {
      if (wait_for(fd, POLLOUT, deadline) != 0)
         return -1;
      ssize_t n = send(fd, data + done, len - done, MSG_NOSIGNAL);
      if (n > 0)
      {
         done += (size_t)n;
         if (written)
            *written = done;
      }
      else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      else
         return -1;
   }
   return 0;
}

static int write_all(int fd, const unsigned char *data, size_t len, int64_t deadline)
{
   return write_all_count(fd, data, len, deadline, NULL);
}

static int read_all(int fd, unsigned char *data, size_t len, int64_t deadline)
{
   size_t done = 0;
   while (done < len)
   {
      if (wait_for(fd, POLLIN, deadline) != 0)
         return -1;
      ssize_t n = recv(fd, data + done, len - done, 0);
      if (n > 0)
         done += (size_t)n;
      else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      else
         return -1;
   }
   return 0;
}

static int read_eof(int fd, int64_t deadline)
{
   unsigned char extra;
   for (;;)
   {
      if (wait_for(fd, POLLIN, deadline) != 0)
         return -1;
      ssize_t n = recv(fd, &extra, 1, 0);
      if (n == 0)
         return 0;
      if (n > 0)
         return -1;
      if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
         return -1;
   }
}

static void put_u16(unsigned char out[2], uint16_t value)
{
   value = htons(value);
   memcpy(out, &value, sizeof(value));
}

static void put_u32(unsigned char out[4], uint32_t value)
{
   value = htonl(value);
   memcpy(out, &value, sizeof(value));
}

static uint16_t get_u16(const unsigned char in[2])
{
   uint16_t value;
   memcpy(&value, in, sizeof(value));
   return ntohs(value);
}

static uint32_t get_u32(const unsigned char in[4])
{
   uint32_t value;
   memcpy(&value, in, sizeof(value));
   return ntohl(value);
}

static int root_owned_parent_chain(const char *path)
{
   char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
   size_t n = strlen(path);
   if (n >= sizeof(parent))
      return 0;
   memcpy(parent, path, n + 1);
   char *slash = strrchr(parent, '/');
   if (!slash)
      return 0;
   if (slash == parent)
      parent[1] = '\0';
   else
      *slash = '\0';
   for (;;)
   {
      struct stat st;
      if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
          (st.st_mode & 022) != 0)
         return 0;
      if (parent[0] == '/' && parent[1] == '\0')
         return 1;
      slash = strrchr(parent, '/');
      if (!slash)
         return 0;
      if (slash == parent)
         parent[1] = '\0';
      else
         *slash = '\0';
   }
}

static int exact_socket(const char *path, uid_t owner, gid_t group, mode_t mode)
{
   struct stat st;
   return valid_path(path) && root_owned_parent_chain(path) && lstat(path, &st) == 0 &&
          S_ISSOCK(st.st_mode) && st.st_uid == owner && st.st_gid == group &&
          (st.st_mode & 07777) == mode;
}

static int connect_checked(const kb_mgmt_token_authority_client_config_t *config)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   if (!config || !exact_socket(config->socket_path, 0, config->socket_gid, config->socket_mode))
      return -1;
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_un address;
   memset(&address, 0, sizeof(address));
   address.sun_family = AF_UNIX;
   size_t path_len = strlen(config->socket_path);
   memcpy(address.sun_path, config->socket_path, path_len + 1);
   socklen_t address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
   int connected = connect(fd, (const struct sockaddr *)&address, address_len);
   if (connected != 0 && errno == EINPROGRESS)
   {
      int64_t now = monotonic_ms();
      int64_t deadline = now < 0 ? -1 : now + config->timeout_ms;
      connected = wait_for(fd, POLLOUT, deadline);
      int socket_error = 0;
      socklen_t error_len = sizeof(socket_error);
      if (connected == 0 &&
          (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 || socket_error))
         connected = -1;
   }
   struct ucred peer;
   socklen_t peer_len = sizeof(peer);
   if (connected != 0 ||
       !exact_socket(config->socket_path, 0, config->socket_gid, config->socket_mode) ||
       getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 || peer_len != sizeof(peer) ||
       peer.uid != 0)
   {
      (void)close(fd);
      return -1;
   }
   return fd;
#else
   (void)config;
   errno = ENOTSUP; /* No credentialless portability fallback is permitted. */
   return -1;
#endif
}

kb_mgmt_token_authority_ipc_result_t
kb_mgmt_token_authority_client_issue(const kb_mgmt_token_authority_client_config_t *config,
                                     const char *correlation_id, const char *jti,
                                     kb_mgmt_token_authority_output_t *out)
{
   clear_output(out);
   const size_t jti_len = KB_MGMT_TOKEN_AUTHORITY_JTI_LEN;
   if (!out || !config || !valid_path(config->socket_path) ||
       strcmp(config->socket_path, KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH) != 0 || getuid() == 0 ||
       geteuid() == 0 || getuid() != geteuid() || !config->timeout_ms ||
       config->timeout_ms > 60000 || config->socket_mode != 0660 ||
       !lower_hex_exact(correlation_id, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN) ||
       !lower_hex_exact(jti, KB_MGMT_TOKEN_AUTHORITY_JTI_LEN))
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID;

   int fd = connect_checked(config);
   if (fd < 0)
      return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   int64_t now = monotonic_ms();
   int64_t deadline = now < 0 ? -1 : now + config->timeout_ms;
   unsigned char request[REQUEST_HEADER_LEN + KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN +
                         KB_MGMT_TOKEN_AUTHORITY_JTI_LEN];
   memset(request, 0, sizeof(request));
   memcpy(request, request_magic, sizeof(request_magic));
   request[4] = 1; /* protocol version */
   request[5] = 1; /* ISSUE */
   put_u16(request + 6, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN);
   put_u16(request + 8, (uint16_t)jti_len);
   put_u16(request + 10, 0); /* flags/reserved */
   size_t request_len = REQUEST_HEADER_LEN + KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN + jti_len;
   memcpy(request + REQUEST_HEADER_LEN, correlation_id, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN);
   memcpy(request + REQUEST_HEADER_LEN + KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN, jti, jti_len);

   size_t request_bytes_sent = 0;
   int request_sent = write_all_count(fd, request, request_len, deadline, &request_bytes_sent) == 0;
   OPENSSL_cleanse(request, sizeof(request));
   if (!request_sent)
   {
      (void)close(fd);
      /* Once any byte crosses the socket, prefer terminal availability loss:
       * future protocol versions must not make a prefix independently usable. */
      return request_bytes_sent ? KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS
                                : KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   }
   unsigned char response[RESPONSE_HEADER_LEN];
   if (shutdown(fd, SHUT_WR) != 0 || read_all(fd, response, sizeof(response), deadline) != 0)
   {
      (void)close(fd);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
   }
   if (memcmp(response, response_magic, sizeof(response_magic)) != 0 || response[4] != 1 ||
       response[5] != 1)
   {
      (void)close(fd);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   }
   uint16_t status = get_u16(response + 6);
   uint32_t body_len = get_u32(response + 8);
   if (status > KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED ||
       (status == KB_MGMT_TOKEN_AUTHORITY_IPC_OK
            ? body_len == 0 || body_len > KB_MGMT_TOKEN_WIRE_MAX
            : body_len != 0))
   {
      (void)close(fd);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   }
   if (body_len && read_all(fd, (unsigned char *)out->jwt, body_len, deadline) != 0)
   {
      clear_output(out);
      (void)close(fd);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
   }
   if (read_eof(fd, deadline) != 0)
   {
      clear_output(out);
      (void)close(fd);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
   }
   (void)close(fd);
   if (status != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      return (kb_mgmt_token_authority_ipc_result_t)status;
   if (!valid_jwt(out->jwt, body_len))
   {
      clear_output(out);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   }
   out->jwt_len = body_len;
   out->jwt[body_len] = '\0';
   return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
}

/* Shared only by the narrow daemon object. Kept out of the installed header so
 * ordinary code cannot grow another wire operation. */
int kb_mgmt_token_authority_ipc_read_request(int fd, uint32_t timeout_ms, char correlation_id[65],
                                             char jti[65])
{
   int64_t now = monotonic_ms();
   int64_t deadline = now < 0 ? -1 : now + timeout_ms;
   unsigned char header[REQUEST_HEADER_LEN];
   if (read_all(fd, header, sizeof(header), deadline) != 0 ||
       memcmp(header, request_magic, sizeof(request_magic)) != 0 || header[4] != 1 ||
       header[5] != 1 || get_u16(header + 6) != KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN ||
       get_u16(header + 10) != 0)
      return -1;
   size_t jti_len = get_u16(header + 8);
   if (jti_len != KB_MGMT_TOKEN_AUTHORITY_JTI_LEN)
      return -1;
   unsigned char body[KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN + KB_MGMT_TOKEN_AUTHORITY_JTI_LEN];
   if (read_all(fd, body, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN + jti_len, deadline) != 0)
      return -1;
   memcpy(correlation_id, body, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN);
   correlation_id[KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN] = '\0';
   memcpy(jti, body + KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN, jti_len);
   jti[jti_len] = '\0';
   OPENSSL_cleanse(body, sizeof(body));
   if (!lower_hex_exact(correlation_id, KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN) ||
       !lower_hex_exact(jti, KB_MGMT_TOKEN_AUTHORITY_JTI_LEN))
      return -1;
   return read_eof(fd, deadline); /* Client must half-close: exactly one request. */
}

int kb_mgmt_token_authority_ipc_write_response(int fd, uint32_t timeout_ms,
                                               kb_mgmt_token_authority_ipc_result_t status,
                                               const kb_mgmt_token_authority_output_t *out)
{
   uint32_t body_len = 0;
   if (status == KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
   {
      if (!out || out->jwt_len > KB_MGMT_TOKEN_WIRE_MAX || out->jwt[out->jwt_len] != '\0' ||
          !valid_jwt(out->jwt, out->jwt_len))
         status = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      else
         body_len = (uint32_t)out->jwt_len;
   }
   else if (status < KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID ||
            status > KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED)
      status = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   unsigned char header[RESPONSE_HEADER_LEN];
   memcpy(header, response_magic, sizeof(response_magic));
   header[4] = 1;
   header[5] = 1;
   put_u16(header + 6, (uint16_t)status);
   put_u32(header + 8, body_len);
   int64_t now = monotonic_ms();
   int64_t deadline = now < 0 ? -1 : now + timeout_ms;
   return write_all(fd, header, sizeof(header), deadline) == 0 &&
                  (!body_len ||
                   write_all(fd, (const unsigned char *)out->jwt, body_len, deadline) == 0)
              ? 0
              : -1;
}

int kb_mgmt_token_authority_ipc_socket_matches(int fd, const char *path, gid_t gid, mode_t mode)
{
   struct sockaddr_un address;
   socklen_t address_len = sizeof(address);
   memset(&address, 0, sizeof(address));
   if (!exact_socket(path, 0, gid, mode) ||
       getsockname(fd, (struct sockaddr *)&address, &address_len) != 0 ||
       address.sun_family != AF_UNIX || address.sun_path[0] == '\0' ||
       strcmp(address.sun_path, path) != 0)
      return 0;
   /* Linux exposes different sockfs and pathname inode identities for one
    * bound Unix socket. The exact kernel-bound pathname plus the independently
    * checked root-owned filesystem node is the non-substitutable contract. */
   return 1;
}

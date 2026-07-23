#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_vault_operator_status.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const unsigned char request_magic[4] = {'A', '7', 'V', 'S'};
static const unsigned char response_magic[4] = {'A', '7', 'V', 'R'};

struct kb_vault_operator_service
{
   pthread_t thread;
   pthread_mutex_t mutex;
   int listen_fd;
   int wake_read_fd;
   int wake_write_fd;
   int stopping;
   kb_vault_operator_status_fn read_status;
   void *opaque;
};

static void put_u16(unsigned char *out, uint16_t value)
{
   out[0] = (unsigned char)(value >> 8);
   out[1] = (unsigned char)value;
}

static void put_u32(unsigned char *out, uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static void put_u64(unsigned char *out, uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      out[i] = (unsigned char)(value >> (56u - i * 8u));
}

static uint16_t get_u16(const unsigned char *in)
{
   return (uint16_t)((uint16_t)in[0] << 8) | in[1];
}

static uint32_t get_u32(const unsigned char *in)
{
   return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

static uint64_t get_u64(const unsigned char *in)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < 8; ++i)
      value = value << 8 | in[i];
   return value;
}

static int all_zero(const unsigned char *data, size_t len)
{
   unsigned char any = 0;
   for (size_t i = 0; i < len; ++i)
      any |= data[i];
   return any == 0;
}

static int64_t monotonic_ms(void)
{
   struct timespec now;
   return clock_gettime(CLOCK_MONOTONIC, &now) == 0
              ? (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000
              : -1;
}

static int deadline_after(uint32_t timeout_ms, int64_t *deadline)
{
   int64_t now = monotonic_ms();
   if (now < 0 || timeout_ms > INT_MAX || now > INT64_MAX - timeout_ms)
      return -1;
   *deadline = now + timeout_ms;
   return 0;
}

static int wait_for(int fd, short events, int64_t deadline)
{
   for (;;)
   {
      int64_t now = monotonic_ms();
      if (now < 0 || now >= deadline)
         return -1;
      int64_t remaining = deadline - now;
      struct pollfd pfd = {.fd = fd, .events = events};
      int rc = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
      if (rc < 0 && errno == EINTR)
         continue;
      if (rc != 1 || (pfd.revents & (POLLERR | POLLNVAL)))
         return -1;
      if ((pfd.revents & events) || ((events & POLLIN) && (pfd.revents & POLLHUP)))
         return 0;
      return -1;
   }
}

static int read_all(int fd, unsigned char *data, size_t len, int64_t deadline)
{
   size_t offset = 0;
   while (offset < len)
   {
      if (wait_for(fd, POLLIN, deadline) != 0)
         return -1;
      ssize_t n = recv(fd, data + offset, len - offset, 0);
      if (n > 0)
         offset += (size_t)n;
      else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      else
         return -1;
   }
   return 0;
}

static int write_all(int fd, const unsigned char *data, size_t len, int64_t deadline)
{
   size_t offset = 0;
   while (offset < len)
   {
      if (wait_for(fd, POLLOUT, deadline) != 0)
         return -1;
      ssize_t n = send(fd, data + offset, len - offset, MSG_NOSIGNAL);
      if (n > 0)
         offset += (size_t)n;
      else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      else
         return -1;
   }
   return 0;
}

static int read_eof(int fd, int64_t deadline)
{
   unsigned char byte;
   for (;;)
   {
      if (wait_for(fd, POLLIN, deadline) != 0)
         return -1;
      ssize_t n = recv(fd, &byte, 1, 0);
      if (n == 0)
         return 0;
      if (n > 0)
         return -1;
      if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
         return -1;
   }
}

static int immediately_has_surplus(int fd)
{
   unsigned char byte;
   for (;;)
   {
      ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
      if (n > 0)
         return 1;
      if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
         return 0;
      if (n < 0 && errno == EINTR)
         continue;
      return -1;
   }
}

static int valid_remediation(const kb_vault_operator_status_t *s)
{
   switch (s->state)
   {
      case KB_VAULT_OPERATOR_STATE_SEALED_IDLE:
      case KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
      case KB_VAULT_OPERATOR_STATE_OPERATIONAL:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_NONE;
      case KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_RESUME ||
                s->remediation == KB_VAULT_OPERATOR_REMEDIATION_BACKEND;
      case KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_RECOVER ||
                s->remediation == KB_VAULT_OPERATOR_REMEDIATION_BACKEND;
      case KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_FINALIZE;
      case KB_VAULT_OPERATOR_STATE_BACKEND_UNAVAILABLE:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_BACKEND;
      case KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE:
         return s->remediation == KB_VAULT_OPERATOR_REMEDIATION_INTEGRITY;
      default:
         return 0; /* unsupported/disabled are reserved and never served in v1 */
   }
}

int kb_vault_operator_status_validate(const kb_vault_operator_status_t *s)
{
   if (!s || s->state < KB_VAULT_OPERATOR_STATE_SEALED_IDLE ||
       s->state > KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE || s->flags > 1 ||
       !s->seal_epoch || !s->control_fence || s->seal_epoch > INT64_MAX ||
       s->control_fence > INT64_MAX || s->old_generation > INT64_MAX ||
       s->new_generation > INT64_MAX || s->last_opened_fence > INT64_MAX ||
       s->last_opened_fence > s->control_fence || !valid_remediation(s))
      return 0;

   if (!(s->flags & 1))
      return s->operation_state == KB_VAULT_OPERATOR_OPERATION_NONE && !s->old_generation &&
             !s->new_generation && all_zero(s->operation_id, sizeof(s->operation_id)) &&
             s->state != KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED &&
             s->state != KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED &&
             s->state != KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED;

   if (s->operation_state < KB_VAULT_OPERATOR_OPERATION_PREPARING ||
       s->operation_state > KB_VAULT_OPERATOR_OPERATION_RECOVERY_REQUIRED ||
       !s->new_generation || s->new_generation != s->old_generation + 1 ||
       all_zero(s->operation_id, sizeof(s->operation_id)))
      return 0;
   if (s->state == KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED)
      return s->operation_state >= KB_VAULT_OPERATOR_OPERATION_PREPARING &&
             s->operation_state <= KB_VAULT_OPERATOR_OPERATION_PROMOTED;
   if (s->state == KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED)
      return s->operation_state == KB_VAULT_OPERATOR_OPERATION_RECOVERY_REQUIRED;
   if (s->state == KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED)
      return s->operation_state == KB_VAULT_OPERATOR_OPERATION_COMPLETED;
   return s->state == KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE;
}

int kb_vault_operator_request_encode(kb_vault_operator_opcode_t opcode, unsigned char out[16])
{
   if (!out || opcode < KB_VAULT_OPERATOR_OPCODE_STATUS || opcode > UINT16_MAX)
      return -1;
   memset(out, 0, 16);
   memcpy(out, request_magic, 4);
   put_u16(out + 4, 1);
   put_u16(out + 6, (uint16_t)opcode);
   return 0;
}

int kb_vault_operator_request_decode(const unsigned char input[16], uint16_t *opcode,
                                     uint32_t *payload_len)
{
   if (opcode)
      *opcode = 0;
   if (payload_len)
      *payload_len = 0;
   if (!input || !opcode || !payload_len || memcmp(input, request_magic, 4) != 0 ||
       get_u16(input + 4) != 1 || get_u32(input + 12) != 0)
      return -1;
   *opcode = get_u16(input + 6);
   *payload_len = get_u32(input + 8);
   /* Opcode validity is deliberately separate from envelope validity.  Zero is
    * not assigned in v1, but it is still a well-formed unknown opcode and must
    * receive the same effectless unsupported response as reserved opcodes. */
   return 0;
}

static void status_encode(const kb_vault_operator_status_t *s, unsigned char out[80])
{
   memset(out, 0, 80);
   put_u16(out, (uint16_t)s->state);
   put_u16(out + 2, (uint16_t)s->operation_state);
   put_u16(out + 4, (uint16_t)s->remediation);
   put_u16(out + 6, s->flags);
   put_u64(out + 8, s->seal_epoch);
   put_u64(out + 16, s->control_fence);
   put_u64(out + 24, s->old_generation);
   put_u64(out + 32, s->new_generation);
   put_u64(out + 40, s->last_opened_fence);
   memcpy(out + 48, s->operation_id, 16);
}

static int status_decode(const unsigned char input[80], kb_vault_operator_status_t *s)
{
   if (!input || !s || !all_zero(input + 64, 16))
      return -1;
   memset(s, 0, sizeof(*s));
   s->state = (kb_vault_operator_state_t)get_u16(input);
   s->operation_state = (kb_vault_operator_operation_state_t)get_u16(input + 2);
   s->remediation = (kb_vault_operator_remediation_t)get_u16(input + 4);
   s->flags = get_u16(input + 6);
   s->seal_epoch = get_u64(input + 8);
   s->control_fence = get_u64(input + 16);
   s->old_generation = get_u64(input + 24);
   s->new_generation = get_u64(input + 32);
   s->last_opened_fence = get_u64(input + 40);
   memcpy(s->operation_id, input + 48, 16);
   return kb_vault_operator_status_validate(s) ? 0 : -1;
}

int kb_vault_operator_response_encode(kb_vault_operator_transport_t result,
                                      const kb_vault_operator_status_t *status,
                                      unsigned char *out, size_t out_cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (!out || !out_len || result < KB_VAULT_OPERATOR_TRANSPORT_OK ||
       result > KB_VAULT_OPERATOR_TRANSPORT_INTERNAL ||
       (result == KB_VAULT_OPERATOR_TRANSPORT_OK && !kb_vault_operator_status_validate(status)) ||
       out_cap < 16u + (result == KB_VAULT_OPERATOR_TRANSPORT_OK ? 80u : 0u))
      return -1;
   memset(out, 0, 16);
   memcpy(out, response_magic, 4);
   put_u16(out + 4, 1);
   put_u16(out + 6, (uint16_t)result);
   put_u32(out + 8, result == KB_VAULT_OPERATOR_TRANSPORT_OK ? 80 : 0);
   *out_len = 16;
   if (result == KB_VAULT_OPERATOR_TRANSPORT_OK)
   {
      status_encode(status, out + 16);
      *out_len += 80;
   }
   return 0;
}

int kb_vault_operator_response_decode(const unsigned char *input, size_t input_len,
                                      kb_vault_operator_transport_t *result,
                                      kb_vault_operator_status_t *status)
{
   if (result)
      *result = KB_VAULT_OPERATOR_TRANSPORT_INTERNAL;
   if (status)
      memset(status, 0, sizeof(*status));
   if (!input || !result || !status || input_len < 16 || memcmp(input, response_magic, 4) != 0 ||
       get_u16(input + 4) != 1 || get_u32(input + 12) != 0)
      return -1;
   uint16_t wire_result = get_u16(input + 6);
   uint32_t payload_len = get_u32(input + 8);
   if (wire_result > KB_VAULT_OPERATOR_TRANSPORT_INTERNAL ||
       (wire_result == KB_VAULT_OPERATOR_TRANSPORT_OK ? payload_len != 80 : payload_len != 0) ||
       input_len != 16u + payload_len)
      return -1;
   *result = (kb_vault_operator_transport_t)wire_result;
   return payload_len ? status_decode(input + 16, status) : 0;
}

static int peer_is_root(int fd)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   struct ucred peer;
   socklen_t peer_len = sizeof(peer);
   return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) == 0 &&
          peer_len == sizeof(peer) && peer.uid == 0;
#else
   (void)fd;
   return 0;
#endif
}

static int write_response(int fd, kb_vault_operator_transport_t result,
                          const kb_vault_operator_status_t *status)
{
   unsigned char response[96];
   size_t response_len = 0;
   int64_t deadline;
   if (kb_vault_operator_response_encode(result, status, response, sizeof(response),
                                         &response_len) != 0 ||
       deadline_after(KB_VAULT_OPERATOR_IO_TIMEOUT_MS, &deadline) != 0)
      return -1;
   return write_all(fd, response, response_len, deadline);
}

int kb_vault_operator_serve_connection(int fd, kb_vault_operator_status_fn read_status,
                                       void *opaque)
{
   if (fd < 0 || !read_status || !peer_is_root(fd))
      return -1; /* Unauthorized peers receive no parse and no response. */
   int flags = fcntl(fd, F_GETFL);
   int fd_flags = fcntl(fd, F_GETFD);
   if (flags < 0 || fd_flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
       fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
      return -1;
   unsigned char request[16];
   int64_t deadline;
   if (deadline_after(KB_VAULT_OPERATOR_IO_TIMEOUT_MS, &deadline) != 0 ||
       read_all(fd, request, sizeof(request), deadline) != 0)
      return -1; /* Truncated/slow header: deliberately silent. */
   uint16_t opcode = 0;
   uint32_t payload_len = 0;
   if (kb_vault_operator_request_decode(request, &opcode, &payload_len) != 0)
      return -1; /* Bad magic/version/reserved: deliberately silent. */
   int surplus = immediately_has_surplus(fd);
   if (payload_len || surplus > 0)
      return write_response(fd, KB_VAULT_OPERATOR_TRANSPORT_BAD_FRAME, NULL);
   if (surplus < 0)
      return -1;
   if (opcode != KB_VAULT_OPERATOR_OPCODE_STATUS)
      return write_response(fd, KB_VAULT_OPERATOR_TRANSPORT_UNSUPPORTED_OPCODE, NULL);

   kb_vault_operator_status_t status;
   memset(&status, 0, sizeof(status));
   if (read_status(&status, opaque) != 0 || !kb_vault_operator_status_validate(&status))
      return write_response(fd, KB_VAULT_OPERATOR_TRANSPORT_INTERNAL, NULL);
   return write_response(fd, KB_VAULT_OPERATOR_TRANSPORT_OK, &status);
}

static int exact_runtime_directory(void)
{
   struct stat st;
   return lstat(KB_VAULT_OPERATOR_RUNTIME_DIR, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
          (st.st_mode & 07777) == 0700;
}

static int exact_socket_path(void)
{
   struct stat st;
   return exact_runtime_directory() && lstat(KB_VAULT_OPERATOR_SOCKET_PATH, &st) == 0 &&
          S_ISSOCK(st.st_mode) && st.st_uid == 0 && (st.st_mode & 07777) == 0600;
}

static int validate_listener(int fd)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   if (fd != KB_VAULT_OPERATOR_LISTEN_FD || !exact_socket_path())
      return 0;
   struct stat st;
   int type = 0;
   int accepting = 0;
   socklen_t option_len = sizeof(type);
   struct sockaddr_un address;
   socklen_t address_len = sizeof(address);
   memset(&address, 0, sizeof(address));
   if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode) ||
       getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &option_len) != 0 ||
       option_len != sizeof(type) || type != SOCK_STREAM)
      return 0;
   option_len = sizeof(accepting);
   if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_len) != 0 ||
       option_len != sizeof(accepting) || !accepting ||
       getsockname(fd, (struct sockaddr *)&address, &address_len) != 0 ||
       address.sun_family != AF_UNIX || address.sun_path[0] == '\0' ||
       strcmp(address.sun_path, KB_VAULT_OPERATOR_SOCKET_PATH) != 0)
      return 0;
   return 1;
#else
   (void)fd;
   return 0;
#endif
}

static int service_stopping(kb_vault_operator_service_t *s)
{
   pthread_mutex_lock(&s->mutex);
   int stopping = s->stopping;
   pthread_mutex_unlock(&s->mutex);
   return stopping;
}

static void *service_main(void *opaque)
{
   kb_vault_operator_service_t *s = opaque;
   while (!service_stopping(s))
   {
      struct pollfd ready[2] = {{.fd = s->listen_fd, .events = POLLIN},
                                {.fd = s->wake_read_fd, .events = POLLIN}};
      int rc = poll(ready, 2, -1);
      if (rc < 0 && errno == EINTR)
         continue;
      if (rc < 0 || (ready[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
         break;
      if (ready[1].revents || service_stopping(s))
         break;
      if (!(ready[0].revents & POLLIN))
         continue;
      int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd < 0)
      {
         if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
         break;
      }
      (void)kb_vault_operator_serve_connection(fd, s->read_status, s->opaque);
      (void)shutdown(fd, SHUT_RDWR);
      (void)close(fd);
   }
   return NULL;
}

kb_vault_operator_service_t *kb_vault_operator_service_start(
    int inherited_fd, kb_vault_operator_status_fn read_status, void *opaque)
{
   if (!read_status || !validate_listener(inherited_fd))
   {
      if (inherited_fd == KB_VAULT_OPERATOR_LISTEN_FD)
         (void)close(inherited_fd);
      return NULL;
   }
   int flags = fcntl(inherited_fd, F_GETFL);
   int fd_flags = fcntl(inherited_fd, F_GETFD);
   if (flags < 0 || fd_flags < 0 ||
       fcntl(inherited_fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
       fcntl(inherited_fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
   {
      (void)close(inherited_fd);
      return NULL;
   }
   int wake[2];
   if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)
   {
      (void)close(inherited_fd);
      return NULL;
   }
   kb_vault_operator_service_t *s = calloc(1, sizeof(*s));
   if (!s)
   {
      close(wake[0]);
      close(wake[1]);
      close(inherited_fd);
      return NULL;
   }
   s->listen_fd = inherited_fd;
   s->wake_read_fd = wake[0];
   s->wake_write_fd = wake[1];
   s->read_status = read_status;
   s->opaque = opaque;
   if (pthread_mutex_init(&s->mutex, NULL) != 0)
   {
      close(wake[0]);
      close(wake[1]);
      close(inherited_fd);
      free(s);
      return NULL;
   }
   if (pthread_create(&s->thread, NULL, service_main, s) != 0)
   {
      pthread_mutex_destroy(&s->mutex);
      close(wake[0]);
      close(wake[1]);
      close(inherited_fd);
      free(s);
      return NULL;
   }
   return s;
}

void kb_vault_operator_service_stop(kb_vault_operator_service_t *s)
{
   if (!s)
      return;
   pthread_mutex_lock(&s->mutex);
   s->stopping = 1;
   pthread_mutex_unlock(&s->mutex);
   unsigned char wake = 1;
   (void)write(s->wake_write_fd, &wake, 1);
   (void)pthread_join(s->thread, NULL); /* Current status callback drains first. */
   (void)close(s->listen_fd);
   (void)close(s->wake_read_fd);
   (void)close(s->wake_write_fd);
   pthread_mutex_destroy(&s->mutex);
   free(s);
}

static int connect_fixed(void)
{
#if defined(__linux__) && defined(SO_PEERCRED)
   if (!exact_socket_path())
      return -1;
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_un address;
   memset(&address, 0, sizeof(address));
   address.sun_family = AF_UNIX;
   memcpy(address.sun_path, KB_VAULT_OPERATOR_SOCKET_PATH,
          sizeof(KB_VAULT_OPERATOR_SOCKET_PATH));
   int64_t deadline;
   int connected = connect(fd, (const struct sockaddr *)&address, sizeof(address));
   if (deadline_after(KB_VAULT_OPERATOR_IO_TIMEOUT_MS, &deadline) != 0)
   {
      close(fd);
      return -1;
   }
   if (connected != 0 && errno == EINPROGRESS && wait_for(fd, POLLOUT, deadline) == 0)
   {
      int socket_error = 0;
      socklen_t error_len = sizeof(socket_error);
      connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
                          error_len == sizeof(socket_error) && socket_error == 0
                      ? 0
                      : -1;
   }
   struct ucred peer;
   socklen_t peer_len = sizeof(peer);
   if (connected != 0 || !exact_socket_path() ||
       getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
       peer_len != sizeof(peer) || peer.uid != 0)
   {
      close(fd);
      return -1;
   }
   return fd;
#else
   errno = ENOTSUP;
   return -1;
#endif
}

kb_vault_operator_client_result_t
kb_vault_operator_status_client(kb_vault_operator_status_t *status)
{
   if (!status)
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   memset(status, 0, sizeof(*status));
   int fd = connect_fixed();
   if (fd < 0)
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   unsigned char request[16];
   unsigned char response[96];
   int64_t deadline;
   int ok = kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, request) == 0 &&
            deadline_after(KB_VAULT_OPERATOR_IO_TIMEOUT_MS, &deadline) == 0 &&
            write_all(fd, request, sizeof(request), deadline) == 0 && shutdown(fd, SHUT_WR) == 0 &&
            read_all(fd, response, 16, deadline) == 0;
   uint32_t payload_len = ok ? get_u32(response + 8) : 0;
   if (ok && payload_len == 80)
      ok = read_all(fd, response + 16, 80, deadline) == 0;
   if (ok)
      ok = read_eof(fd, deadline) == 0;
   close(fd);
   kb_vault_operator_transport_t result;
   if (!ok || kb_vault_operator_response_decode(response, 16u + payload_len, &result, status) != 0 ||
       result != KB_VAULT_OPERATOR_TRANSPORT_OK)
   {
      memset(status, 0, sizeof(*status));
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   }
   return kb_vault_operator_status_exit_mapping(status);
}

kb_vault_operator_client_result_t
kb_vault_operator_status_exit_mapping(const kb_vault_operator_status_t *status)
{
   if (!kb_vault_operator_status_validate(status))
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   if (status->state == KB_VAULT_OPERATOR_STATE_OPERATIONAL)
      return KB_VAULT_OPERATOR_CLIENT_OK;
   if (status->state == KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE)
      return KB_VAULT_OPERATOR_CLIENT_INTEGRITY_FAILURE;
   return KB_VAULT_OPERATOR_CLIENT_ACTION_REQUIRED;
}

int kb_vault_operator_startup_mode(kb_vault_operator_state_t state)
{
   switch (state)
   {
      case KB_VAULT_OPERATOR_STATE_OPERATIONAL:
         return 0;
      case KB_VAULT_OPERATOR_STATE_SEALED_IDLE:
      case KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED:
      case KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED:
      case KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED:
      case KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED:
         return 1;
      case KB_VAULT_OPERATOR_STATE_UNSUPPORTED_RESERVED:
      case KB_VAULT_OPERATOR_STATE_DISABLED_RESERVED:
      case KB_VAULT_OPERATOR_STATE_BACKEND_UNAVAILABLE:
      case KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE:
         return -1;
   }
   return -1;
}

const char *kb_vault_operator_state_name(kb_vault_operator_state_t state)
{
   static const char *const names[] = {NULL,
                                      "unsupported",
                                      "disabled",
                                      "sealed_idle",
                                      "operational",
                                      "local_unseal_required",
                                      "resume_required",
                                      "recovery_required",
                                      "completed_sealed",
                                      "backend_unavailable",
                                      "integrity_failure"};
   return state >= 1 && state <= 10 ? names[state] : NULL;
}

const char *kb_vault_operator_operation_name(kb_vault_operator_operation_state_t state)
{
   static const char *const names[] = {"none",          "preparing", "custody_prepared",
                                      "wraps_staged",  "reseal_committing", "resealed",
                                      "promoted",      "completed", "aborted",
                                      "recovery_required"};
   return state >= 0 && state <= 9 ? names[state] : NULL;
}

const char *kb_vault_operator_remediation_name(kb_vault_operator_remediation_t remediation)
{
   static const char *const names[] = {"none",    "configure", "unseal",   "resume", "recover",
                                      "upgrade", "backend",   "integrity", "finalize"};
   return remediation >= 0 && remediation <= 8 ? names[remediation] : NULL;
}

int kb_vault_operator_status_format(const kb_vault_operator_status_t *status, int json, char *out,
                                    size_t out_cap)
{
   if (!out || !out_cap || !kb_vault_operator_status_validate(status))
      return -1;
   char operation_id[33];
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < 16; ++i)
   {
      operation_id[i * 2] = hex[status->operation_id[i] >> 4];
      operation_id[i * 2 + 1] = hex[status->operation_id[i] & 15];
   }
   operation_id[32] = '\0';
   int n;
   if (json)
      n = snprintf(out, out_cap,
                   "{\"state\":\"%s\",\"operation_state\":\"%s\",\"remediation\":\"%s\","
                   "\"operation_present\":%s,\"seal_epoch\":%llu,\"control_fence\":%llu,"
                   "\"old_generation\":%llu,\"new_generation\":%llu,"
                   "\"last_opened_fence\":%llu,\"operation_id\":%s%s%s}\n",
                   kb_vault_operator_state_name(status->state),
                   kb_vault_operator_operation_name(status->operation_state),
                   kb_vault_operator_remediation_name(status->remediation),
                   status->flags & 1 ? "true" : "false", (unsigned long long)status->seal_epoch,
                   (unsigned long long)status->control_fence,
                   (unsigned long long)status->old_generation,
                   (unsigned long long)status->new_generation,
                   (unsigned long long)status->last_opened_fence, status->flags & 1 ? "\"" : "",
                   status->flags & 1 ? operation_id : "null", status->flags & 1 ? "\"" : "");
   else
      n = snprintf(out, out_cap, "vault=%s remediation=%s operation=%s\n",
                   kb_vault_operator_state_name(status->state),
                   kb_vault_operator_remediation_name(status->remediation),
                   kb_vault_operator_operation_name(status->operation_state));
   return n >= 0 && (size_t)n < out_cap ? n : -1;
}

#ifndef AIMEE_CORE_CONNECTION_SOCKET_H
#define AIMEE_CORE_CONNECTION_SOCKET_H 1

#include <stddef.h>
#include <aimee/core/connection/control.h>

#ifndef _WIN32
struct addrinfo;
#endif

#ifdef __cplusplus
extern "C"
{
#endif

   /* Portable blocking TCP stream used by every networked Aimee C product.
    * The connect timeout bounds each address attempt; <= 0 selects 10 seconds. */
   int aimee_core_socket_connect(const char *host, const char *port, int timeout_ms);

   enum
   {
      AIMEE_CORE_CONNECT_NUMERIC_HOST = 1U << 0,
      AIMEE_CORE_CONNECT_NONBLOCKING = 1U << 1
   };
   /* One candidate address's share of the remaining connect budget. Exposed for
      the unit test: the starvation it prevents cannot be reproduced portably
      through the socket path, because that needs a first address that HANGS and
      a test can only conjure one that REFUSES -- which the broken code handled
      correctly. */
   int aimee_core_connect_slice_ms(int remaining_ms, int candidates, int attempted);

   aimee_core_result_t aimee_core_socket_connect_controlled(const char *host, const char *port,
                                                            unsigned flags,
                                                            const aimee_core_control_t *control,
                                                            int *out_fd);
#ifndef _WIN32
   aimee_core_result_t aimee_core_socket_connect_addresses(const struct addrinfo *addresses,
                                                           unsigned flags,
                                                           const aimee_core_control_t *control,
                                                           int *out_fd);
#endif

   /* Apply blocking send/receive timeouts to an established stream. A
    * non-positive timeout disables that direction's timeout. */
   int aimee_core_socket_set_timeouts(int fd, int receive_timeout_ms, int send_timeout_ms);

   /* Send the complete buffer, suppressing SIGPIPE where available. Controlled
    * I/O requires a nonblocking descriptor so its deadline/cancellation source
    * can interrupt readiness waits; controlled connect can return one. */
   int aimee_core_socket_write_all(int fd, const void *buffer, size_t length);
   aimee_core_result_t aimee_core_socket_write_all_controlled(int fd, const void *buffer,
                                                              size_t length,
                                                              const aimee_core_control_t *control,
                                                              size_t *bytes_written);

   /* Read up to length bytes. Returns zero on orderly close and -1 on error. */
   long aimee_core_socket_read(int fd, void *buffer, size_t length);
   aimee_core_result_t aimee_core_socket_read_controlled(int fd, void *buffer, size_t length,
                                                         const aimee_core_control_t *control,
                                                         size_t *bytes_read);

   /* Returns 1 when readable, 0 on timeout, and -1 on error/hangup. */
   int aimee_core_socket_wait_readable(int fd, int timeout_ms);

   void aimee_core_socket_close(int fd);

#ifdef __cplusplus
}
#endif

#endif

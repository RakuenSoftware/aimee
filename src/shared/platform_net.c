/* Compatibility facade. The implementation is owned by libaimee-core-connection. */
#include "platform_net.h"
#include <aimee/core/connection/socket.h>

int platform_net_connect(const char *host, const char *port, int timeout_ms)
{
   return aimee_core_socket_connect(host, port, timeout_ms);
}

int platform_net_send_all(int fd, const void *buf, size_t len)
{
   return aimee_core_socket_write_all(fd, buf, len);
}

long platform_net_recv(int fd, void *buf, size_t len)
{
   return aimee_core_socket_read(fd, buf, len);
}

void platform_net_close(int fd)
{
   aimee_core_socket_close(fd);
}

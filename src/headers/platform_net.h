/*
 * platform_net.h: portable TCP client abstraction.
 *
 * Linux/macOS use BSD sockets; Windows uses Winsock. Provides a blocking
 * connect/send/recv/close over a TCP stream, identified by an int descriptor
 * (on Windows the SOCKET is cast through intptr_t, mirroring platform_ipc).
 *
 * Compatibility facade for older call sites. The implementation now lives in
 * libaimee-core-connection's public aimee/core/connection/socket.h API.
 */
#ifndef DEC_PLATFORM_NET_H
#define DEC_PLATFORM_NET_H 1

#include "platform.h"
#include <stddef.h>

/* Connect a TCP stream to |host|:|port| (port is a decimal string or service
 * name). Resolves via getaddrinfo (IPv4/IPv6) and tries addresses in order.
 * |timeout_ms| bounds each connect attempt (<= 0 means a sane default).
 * Returns a descriptor >= 0 on success, -1 on error. */
int platform_net_connect(const char *host, const char *port, int timeout_ms);

/* Send exactly |len| bytes (loops over partial writes). Returns 0 on success,
 * -1 on error. */
int platform_net_send_all(int fd, const void *buf, size_t len);

/* Read up to |len| bytes into |buf|. Returns the number read (0 on orderly
 * close), or -1 on error. */
long platform_net_recv(int fd, void *buf, size_t len);

/* Close a descriptor returned by platform_net_connect. */
void platform_net_close(int fd);

#endif /* DEC_PLATFORM_NET_H */

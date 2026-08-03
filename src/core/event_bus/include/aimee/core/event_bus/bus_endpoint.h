#ifndef AIMEE_CORE_EVENT_BUS_ENDPOINT_H
#define AIMEE_CORE_EVENT_BUS_ENDPOINT_H 1

/* Local SOCK_SEQPACKET endpoint used only for the attach handshake. Event data
 * moves through the shared-memory regions granted during that handshake.
 * Listen refuses an existing path; remove a known-stale socket explicitly. */
int bus_endpoint_listen(const char *path, unsigned mode, int backlog, int *out_fd);
int bus_endpoint_accept(int listener_fd, int *out_fd);
int bus_endpoint_connect(const char *path, int *out_fd);
int bus_endpoint_close(int *fd);
int bus_endpoint_remove(const char *path);

#endif

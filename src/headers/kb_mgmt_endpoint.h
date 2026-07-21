#ifndef KB_MGMT_ENDPOINT_H
#define KB_MGMT_ENDPOINT_H
#include <stddef.h>
#include <sys/socket.h>

typedef struct
{
   char host[254];
   char host_header[320];
   int port;
} kb_mgmt_endpoint_t;

int kb_mgmt_endpoint_parse(const char *endpoint, kb_mgmt_endpoint_t *out);
int kb_mgmt_endpoint_validate(const char *endpoint);
int kb_mgmt_sockaddr_permitted(const struct sockaddr *addr, socklen_t len);
int kb_mgmt_endpoint_connect(const kb_mgmt_endpoint_t *endpoint);
#endif

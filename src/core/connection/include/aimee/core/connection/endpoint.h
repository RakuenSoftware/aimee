#ifndef AIMEE_CORE_CONNECTION_ENDPOINT_H
#define AIMEE_CORE_CONNECTION_ENDPOINT_H 1

#include <stddef.h>

#define AIMEE_COMM_HOST_MAX 255
#define AIMEE_COMM_PORT_MAX 15

typedef struct
{
   char host[AIMEE_COMM_HOST_MAX + 1];
   char port[AIMEE_COMM_PORT_MAX + 1];
   int secure;
} aimee_core_endpoint_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse http[s]://host[:port][/...] or the legacy host[:port] form. IPv6
    * literals must use URL brackets. Missing ports default to 80/443. */
   int aimee_core_endpoint_parse(const char *value, aimee_core_endpoint_t *out);

#ifdef __cplusplus
}
#endif

#endif

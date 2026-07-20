#ifndef KB_MGMT_CLIENT_H
#define KB_MGMT_CLIENT_H
#include <stddef.h>
int kb_mgmt_client_request(const char *endpoint, const char *ca, const char *client_cert,
                           const char *client_key, const char *method, const char *path,
                           const char *body, char *resp, size_t cap, int *status);
#endif

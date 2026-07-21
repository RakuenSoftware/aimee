#ifndef KB_HTTP_EGRESS_H
#define KB_HTTP_EGRESS_H

#include "kb_identity.h"

int kb_http_egress_route(const char *method, const char *path, const char *body, int body_len,
                         const kb_principal_t *transport, const char *fingerprint, char *out,
                         int out_cap);

#endif

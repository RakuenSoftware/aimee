#ifndef AIMEE_KB_STORE_MANAGEMENT_TOKEN_ROOTS_H
#define AIMEE_KB_STORE_MANAGEMENT_TOKEN_ROOTS_H

#include "kb_mgmt_token_roots_provision.h"

#include <stddef.h>

typedef struct
{
   void *connection;
   int session_lock_held;
} kb_store_management_token_roots_ctx_t;

int kb_store_management_token_roots_open(kb_store_management_token_roots_ctx_t *,
                                         const char *conninfo, char *errbuf, size_t errlen);
void kb_store_management_token_roots_close(kb_store_management_token_roots_ctx_t *);

/* Populate the provisioner's narrow database seam. The returned seam borrows
 * ctx and is invalid after close. */
int kb_store_management_token_roots_bind(kb_store_management_token_roots_ctx_t *,
                                         kb_mgmt_roots_db_t *);

#endif

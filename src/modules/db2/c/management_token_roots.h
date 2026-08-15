#ifndef AIMEE_DB2_MANAGEMENT_TOKEN_ROOTS_H
#define AIMEE_DB2_MANAGEMENT_TOKEN_ROOTS_H

#include "../kb/kb_mgmt_token_roots_provision.h"

#include <stddef.h>

typedef struct
{
   void *connection;
   int session_lock_held;
} db2_management_token_roots_ctx_t;

int db2_management_token_roots_open(db2_management_token_roots_ctx_t *, const char *conninfo,
                                    char *errbuf, size_t errlen);
void db2_management_token_roots_close(db2_management_token_roots_ctx_t *);

/* Populate the provisioner's narrow database seam. The returned seam borrows
 * ctx and is invalid after close. */
int db2_management_token_roots_bind(db2_management_token_roots_ctx_t *, kb_mgmt_roots_db_t *);

#endif

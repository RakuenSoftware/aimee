#ifndef AIMEE_MANAGED_SERVER_IDENTITY_INSTALL_H
#define AIMEE_MANAGED_SERVER_IDENTITY_INSTALL_H

#include <sys/types.h>

typedef struct
{
   const char *server_home;
   const char *host;
   int port;
   const char *endpoint;
   uid_t owner;
} kb_managed_server_identity_install_options_t;

/* Install or resume the wizard-managed server's durable mTLS identity. DB2 must
 * already be initialized. Prints only non-secret readiness metadata on success. */
int kb_managed_server_identity_install(
    const kb_managed_server_identity_install_options_t *options);

#endif

#ifndef AIMEE_MANAGED_SERVER_IDENTITY_INSTALL_H
#define AIMEE_MANAGED_SERVER_IDENTITY_INSTALL_H

#include <sys/types.h>

typedef struct
{
   const char *server_home;
   const char *host;
   int port;
   const char *endpoint;
   /* Optional host/PAM operator to enroll into the managed server's team. The
    * server resolves this from its root-owned onboarding record; the KB still
    * performs the membership mutation under its bootstrap owner authority. */
   const char *member;
   uid_t owner;
   /* Re-issue the client certificate even when a stored identity already
    * matches this KB. Without it the installer reuses whatever is on disk and
    * reports readiness, so an identity the KB no longer accepts — a rotated CA,
    * a revoked or expired cert, a lost registry row — has no supported repair. */
   int force;
} kb_managed_server_identity_install_options_t;

/* Install or resume the wizard-managed server's durable mTLS identity. DB2 must
 * already be initialized. Prints only non-secret readiness metadata on success. */
int kb_managed_server_identity_install(const kb_managed_server_identity_install_options_t *options);

#endif

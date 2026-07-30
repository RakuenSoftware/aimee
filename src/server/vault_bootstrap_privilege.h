#ifndef AIMEE_VAULT_BOOTSTRAP_PRIVILEGE_H
#define AIMEE_VAULT_BOOTSTRAP_PRIVILEGE_H

#ifndef AIMEE_WINDOWS
#include <sys/types.h>

/* Testable path-specific form of the legacy container-volume repair. */
int vault_bootstrap_repair_owner_at(const char *home, uid_t uid, gid_t gid);
#endif

/* Parse the complete aimee-server bootstrap argv. */
int vault_bootstrap_parse_args(int argc, char **argv, const char **drop_user);

/* Resolve user, repair the legacy Vault while privileged, then drop forever. */
int vault_bootstrap_run_as(const char *user);

#endif

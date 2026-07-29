#ifndef AIMEE_VAULT_ENV_BOOTSTRAP_H
#define AIMEE_VAULT_ENV_BOOTSTRAP_H 1

/* Seal every credential-shaped environment variable into the local
 * server-principal Vault, populate runtime_secret from Vault, and unset the
 * environment values. Safe and idempotent on both aimee-server and aimee-kb.
 * Returns the number newly sealed, or -1 on any fail-closed error. */
int vault_env_bootstrap_init(void);

/* Pure classifier shared with tests and the source guard. */
int vault_env_name_is_credential(const char *name);

#endif /* AIMEE_VAULT_ENV_BOOTSTRAP_H */

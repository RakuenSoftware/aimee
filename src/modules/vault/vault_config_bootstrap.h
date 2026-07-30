#ifndef AIMEE_VAULT_CONFIG_BOOTSTRAP_H
#define AIMEE_VAULT_CONFIG_BOOTSTRAP_H 1

/* Migrate credential-bearing legacy aimee.yaml fields into the encrypted local
 * server-principal Vault, remove them from aimee.yaml, and hydrate the bounded
 * runtime cache. Returns 0 on success and fails closed if either sealing or
 * removing the plaintext config value fails. */
int vault_config_bootstrap_init(void);

/* Persist a runtime credential only in the encrypted Vault and refresh its
 * process-memory copy. `name` uses the first-boot environment name so every
 * credential has one stable lookup key. */
int vault_runtime_secret_set(const char *name, const char *value);
int vault_runtime_secret_delete(const char *name);

#endif /* AIMEE_VAULT_CONFIG_BOOTSTRAP_H */

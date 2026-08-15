#ifndef AIMEE_VAULT_ENV_BOOTSTRAP_H
#define AIMEE_VAULT_ENV_BOOTSTRAP_H 1

#include <stdio.h>

/* Seal every credential-shaped environment variable into the local
 * server-principal Vault, populate runtime_secret from Vault, and unset the
 * environment values. Safe and idempotent on both aimee-server and aimee-kb.
 * Returns the number newly sealed, or -1 on any fail-closed error. */
int vault_env_bootstrap_init(void);
int vault_env_bootstrap_init_all(void);

/* Import NUL-delimited NAME=VALUE records (the `env -0` format) from a
 * disposable first-boot stream into this short-lived process. Only names
 * classified as credentials are imported; values are never printed. The
 * caller must immediately run the normal Vault bootstrap and exit. Returns
 * the number imported, or -1 on malformed/oversized/duplicate input. */
int vault_env_import_stream(FILE *input);

/* Pure classifier shared with tests and the source guard. */
int vault_env_name_is_credential(const char *name);
int vault_env_name_is_any_credential(const char *name);

/* Return 1 when the current process inherited any credential-shaped variable,
 * 0 when clean, or -1 when a malformed/oversized name must fail closed. */
int vault_env_has_credential_environment(void);

/* Emit shell-safe credential variable names, one per line, never values. */
int vault_env_print_credential_names(void);

/* Container-only webchat bridge. Values are read from the encrypted
 * server-principal Vault and emitted as labelled base64 records so the root
 * web service can authenticate through a pipe without ever recreating a
 * plaintext credential file or environment variable. */
int vault_env_print_webchat_bootstrap(void);

/* Validate that the fixed Vault records contain a complete usable first login.
 * Emits no secret and is used to fail container startup before services launch. */
int vault_env_check_webchat_bootstrap(void);

/* Seal one legacy/onboarding webchat record supplied on stdin. The accepted
 * record names are deliberately closed; this is not a general secret-printing
 * or arbitrary-vault-write CLI. */
int vault_env_seal_webchat_record(const char *record_name);

/* Seal one server-principal git credential supplied on stdin, so a deployment
 * that missed the first-boot AIMEE_FORGE_TOKEN can be provisioned without being
 * torn down and re-created.
 *
 * The forge token is only ever readable through git_forge_vault_server_token
 * (the server principal's own vault), so vault_service_set is not a substitute:
 * it stores under the CALLING principal, where the forge reader never looks.
 * Before this existed, first-boot env was the single writer of that entry.
 *
 * Same shape as the webchat seal, for the same reasons: the secret travels on
 * stdin (never argv or env, so it cannot leak through /proc or a process list),
 * and the accepted credential names are a closed allowlist. Returns 0 on
 * success, -1 on a rejected name or a fail-closed Vault error. */
int vault_env_seal_forge_credential(const char *cred_name);

#endif /* AIMEE_VAULT_ENV_BOOTSTRAP_H */

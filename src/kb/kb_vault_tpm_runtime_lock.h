#ifndef AIMEE_KB_VAULT_TPM_RUNTIME_LOCK_H
#define AIMEE_KB_VAULT_TPM_RUNTIME_LOCK_H 1

#include <stddef.h>

#define KB_VAULT_TPM_RUNTIME_LOCK_DIR "/run/aimee/vault-tpm2-locks"

typedef struct kb_vault_tpm_runtime_lock kb_vault_tpm_runtime_lock_t;

/* Resolve the exact env-over-config identity used by the TPM2 provider.  The
 * returned pointers remain owned by the environment/config caller. */
void kb_vault_tpm_runtime_identity(const char *configured_tcti, const char *configured_nv_index,
                                   const char **effective_tcti, const char **effective_nv_index);

typedef enum
{
   KB_VAULT_TPM_RUNTIME_LOCK_OK = 0,
   KB_VAULT_TPM_RUNTIME_LOCK_INVALID = -1,
   KB_VAULT_TPM_RUNTIME_LOCK_UNSUPPORTED = -2,
   KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE = -3,
   KB_VAULT_TPM_RUNTIME_LOCK_IO = -4,
   KB_VAULT_TPM_RUNTIME_LOCK_BUSY = -5,
   KB_VAULT_TPM_RUNTIME_LOCK_LOST = -6,
} kb_vault_tpm_runtime_lock_result_t;

/* Acquire the daemon-lifetime singleton for one canonical TPM NV index.
 * Production accepts only device:/dev/tpmrm0 and a lowercase, zero-padded
 * canonical NV index (0x01xxxxxx).  The fixed runtime directory and lock file
 * must be root-owned, non-symlink objects with exact 0700/0600 modes. */
kb_vault_tpm_runtime_lock_result_t
kb_vault_tpm_runtime_lock_acquire(const char *tcti, const char *nv_index,
                                  kb_vault_tpm_runtime_lock_t **out, char *errbuf, size_t errlen);

/* Revalidate inode identity, ownership/mode, FD_CLOEXEC and nonblocking flock.
 * Call before custody initialization and before listener activation. Acquisition
 * registers a parent atfork hook that performs the same full check after every
 * fork automatically and exits fail-closed if ownership was lost. */
kb_vault_tpm_runtime_lock_result_t
kb_vault_tpm_runtime_lock_revalidate(kb_vault_tpm_runtime_lock_t *owner);

/* Acquisition centrally registers a pthread_atfork child hook that invokes this
 * close, so helper call sites do not know or audit the fd.  The explicit entry
 * is idempotent for test/raw child paths; it is async-signal-safe and does not
 * free inherited memory. The child must exec or _exit afterwards. */
void kb_vault_tpm_runtime_lock_after_fork_child(kb_vault_tpm_runtime_lock_t *owner);

/* Parent lifecycle release.  Idempotent and nulls the caller's opaque owner. */
void kb_vault_tpm_runtime_lock_release(kb_vault_tpm_runtime_lock_t **owner);

#endif

#ifndef AIMEE_KB_VAULT_TPM_RUNTIME_LOCK_TEST_H
#define AIMEE_KB_VAULT_TPM_RUNTIME_LOCK_TEST_H 1

#include "kb_vault_tpm_runtime_lock.h"
#include <sys/types.h>

/* Test-only directory injection.  Production has no path/owner/build override.
 * runtime_dir_fd is duplicated and remains caller-owned. */
kb_vault_tpm_runtime_lock_result_t kb_vault_tpm_runtime_lock_acquire_at_for_test(
    int runtime_dir_fd, uid_t expected_uid, int build_supported, int allow_loopback_swtpm,
    const char *tcti, const char *nv_index, kb_vault_tpm_runtime_lock_t **out, char *errbuf,
    size_t errlen);

#endif

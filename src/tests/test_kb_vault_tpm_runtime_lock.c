#include "kb/kb_vault_tpm_runtime_lock.h"
#include "kb/kb_vault_tpm_runtime_lock_test.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static int make_dir(char path[128])
{
   snprintf(path, 128, "%s/aimee-p7d3-lock-XXXXXX", platform_tmpdir());
   assert(mkdtemp(path));
   assert(chmod(path, 0700) == 0);
   int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   assert(fd >= 0);
   return fd;
}

int main(void)
{
   char path[128], err[160];
   const char *effective_tcti = NULL, *effective_nv_index = NULL;
   assert(unsetenv("AIMEE_VAULT_TPM2_TCTI") == 0);
   assert(unsetenv("AIMEE_VAULT_TPM2_NV_INDEX") == 0);
   kb_vault_tpm_runtime_identity("device:/dev/tpmrm0", "0x01500001", &effective_tcti,
                                 &effective_nv_index);
   assert(!strcmp(effective_tcti, "device:/dev/tpmrm0"));
   assert(!strcmp(effective_nv_index, "0x01500001"));
   assert(setenv("AIMEE_VAULT_TPM2_TCTI", "swtpm:host=127.0.0.1,port=2321", 1) == 0);
   assert(setenv("AIMEE_VAULT_TPM2_NV_INDEX", "0x01500009", 1) == 0);
   kb_vault_tpm_runtime_identity("device:/dev/tpmrm0", "0x01500001", &effective_tcti,
                                 &effective_nv_index);
   assert(!strcmp(effective_tcti, "swtpm:host=127.0.0.1,port=2321"));
   assert(!strcmp(effective_nv_index, "0x01500009"));
   assert(unsetenv("AIMEE_VAULT_TPM2_TCTI") == 0);
   assert(unsetenv("AIMEE_VAULT_TPM2_NV_INDEX") == 0);

   int dir = make_dir(path);
   uid_t uid = geteuid();
   kb_vault_tpm_runtime_lock_t *first = NULL, *second = NULL;

   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 0, 0, "device:/dev/tpmrm0",
                                                        "0x01500001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_UNSUPPORTED);
   assert(first == NULL);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(
              dir, uid, 1, 0, "swtpm:host=127.0.0.1,port=2321", "0x01500001", &first, err,
              sizeof(err)) == KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "22020097", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0X01500001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0x02000001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE);

   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0x01500001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_OK);
   assert(kb_vault_tpm_runtime_lock_revalidate(first) == KB_VAULT_TPM_RUNTIME_LOCK_OK);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0x01500001", &second, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_BUSY);
   assert(second == NULL);

   /* The registered child hook closes inherited fds without a call site knowing
    * them. Parent ownership remains live; a different NV identity is independent. */
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      assert(kb_vault_tpm_runtime_lock_revalidate(first) == KB_VAULT_TPM_RUNTIME_LOCK_LOST);
      assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(
                 dir, uid, 1, 0, "device:/dev/tpmrm0", "0x01500001", &second, err, sizeof(err)) ==
             KB_VAULT_TPM_RUNTIME_LOCK_BUSY);
      assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(
                 dir, uid, 1, 0, "device:/dev/tpmrm0", "0x01500002", &second, err, sizeof(err)) ==
             KB_VAULT_TPM_RUNTIME_LOCK_OK);
      kb_vault_tpm_runtime_lock_release(&second);
      _exit(0);
   }
   int status = 0;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   assert(kb_vault_tpm_runtime_lock_revalidate(first) == KB_VAULT_TPM_RUNTIME_LOCK_OK);
   assert(unlinkat(dir, "nv-01500002.lock", 0) == 0);

   /* A helper-launch fork cannot return to a parent whose lock pathname was
    * replaced. Isolate the expected fail-closed _exit(1) from this test runner. */
   pid_t failclosed = fork();
   assert(failclosed >= 0);
   if (failclosed == 0)
   {
      kb_vault_tpm_runtime_lock_t *isolated = NULL;
      assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(
                 dir, uid, 1, 0, "device:/dev/tpmrm0", "0x01500003", &isolated, err, sizeof(err)) ==
             KB_VAULT_TPM_RUNTIME_LOCK_OK);
      assert(unlinkat(dir, "nv-01500003.lock", 0) == 0);
      int planted = openat(dir, "nv-01500003.lock", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
      assert(planted >= 0);
      close(planted);
      pid_t should_not_return = fork();
      if (should_not_return == 0)
         _exit(0);
      _exit(99);
   }
   assert(waitpid(failclosed, &status, 0) == failclosed && WIFEXITED(status) &&
          WEXITSTATUS(status) == 1);
   assert(unlinkat(dir, "nv-01500003.lock", 0) == 0);

   /* Replacing the pathname cannot transfer ownership silently. */
   assert(unlinkat(dir, "nv-01500001.lock", 0) == 0);
   int replacement = openat(dir, "nv-01500001.lock", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
   assert(replacement >= 0);
   close(replacement);
   assert(kb_vault_tpm_runtime_lock_revalidate(first) == KB_VAULT_TPM_RUNTIME_LOCK_LOST);
   kb_vault_tpm_runtime_lock_release(&first);

   assert(unlinkat(dir, "nv-01500001.lock", 0) == 0);
   assert(symlinkat("target", dir, "nv-01500001.lock") == 0);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0x01500001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_IO);
   assert(unlinkat(dir, "nv-01500001.lock", 0) == 0);

   assert(fchmod(dir, 0755) == 0);
   assert(kb_vault_tpm_runtime_lock_acquire_at_for_test(dir, uid, 1, 0, "device:/dev/tpmrm0",
                                                        "0x01500001", &first, err, sizeof(err)) ==
          KB_VAULT_TPM_RUNTIME_LOCK_IO);
   close(dir);
   assert(rmdir(path) == 0);
   puts("kb vault TPM runtime lock tests passed");
   return 0;
}

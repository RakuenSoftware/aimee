/* test_vault_tpm2_stub.c: P7-tpm2a default-build (no libtss2) unit test.
 *
 * On a build WITHOUT WITH_TPM2 the tpm2 custody provider is a fail-closed STUB.
 * This test pins that the stub is SAFE: selecting vault.custody=tpm2 binds a
 * provider, but that provider boots SEALED, never yields a KEK, and never flips
 * the §3 live-key gate — so a libtss2-less build cannot accidentally enable live
 * keys on the tpm2 anchor. (The real ESAPI seal/unseal behavior is validated on
 * swtpm by scripts/p7_tpm2_swtpm_test.sh, which builds WITH_TPM2=1.)
 *
 * NOTE: built ONLY in the default configuration; the assertions below encode the
 * stub contract (is_sealed==1 always, get_kek==-1, provision==-1). */
#include "kb/kb_vault_policy.h"
#include "vault_crypto.h"
#include "vault_custody_tpm2.h"
#include "vault_internal.h"
#include "vault_server_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_stub_provider_fail_closed(void)
{
   const vault_custody_provider_t *p = vault_custody_tpm2_provider();
   assert(p != NULL);
   assert(strcmp(p->name, "tpm2") == 0);
   assert(p->get_kek && p->is_sealed && p->unseal && p->seal && p->rotate);

   /* Stub boots + stays SEALED forever. */
   assert(p->is_sealed(p->ctx) == 1);

   /* get_kek fails and cleanses its output buffer. */
   uint8_t kek[VAULT_KEK_LEN];
   memset(kek, 0x5a, sizeof(kek));
   assert(p->get_kek(p->ctx, kek) == -1);

   /* unseal cannot succeed without libtss2; is_sealed unchanged. */
   assert(p->unseal(p->ctx, "any-secret", 10) == -1);
   assert(p->is_sealed(p->ctx) == 1);

   /* seal is a no-op success (already sealed). */
   assert(p->seal(p->ctx) == 0);
   assert(p->is_sealed(p->ctx) == 1);

   /* rotate is refused with a message. */
   char err[128] = {0};
   assert(p->rotate(p->ctx, "srv", NULL, NULL, NULL, 0, err, sizeof(err)) == -1);
   assert(err[0] != '\0');
   printf("  PASS: test_stub_provider_fail_closed\n");
}

static void test_stub_provision_refused(void)
{
   uint8_t kek[VAULT_KEK_LEN];
   memset(kek, 0x11, sizeof(kek));
   char err[128] = {0};
   assert(vault_custody_tpm2_provision(kek, "secret", err, sizeof(err)) == -1);
   assert(strstr(err, "without TPM2 support") != NULL);
   printf("  PASS: test_stub_provision_refused\n");
}

static void test_stub_prepared_reseal_refused(void)
{
   uint8_t op[16], kek[VAULT_KEK_LEN];
   memset(op, 0x22, sizeof(op));
   memset(kek, 0x33, sizeof(kek));
   vault_tpm2_reseal_receipt_t receipt;
   memset(&receipt, 0x5a, sizeof(receipt));
   assert(vault_custody_tpm2_reseal_prepare(op, 1, kek, "secret", &receipt) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   const uint8_t zero[sizeof(receipt)] = {0};
   assert(memcmp(&receipt, zero, sizeof(receipt)) == 0);
   vault_tpm2_reseal_status_t status = VAULT_TPM2_RESEAL_CORRUPT;
   memset(&receipt, 0x5a, sizeof(receipt));
   assert(vault_custody_tpm2_reseal_discover(op, 1, "secret", &receipt, &status) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   assert(memcmp(&receipt, zero, sizeof(receipt)) == 0);
   assert(status == VAULT_TPM2_RESEAL_ABSENT);
   memset(kek, 0x5a, sizeof(kek));
   assert(vault_custody_tpm2_reseal_recover_kek(&receipt, "secret", kek) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   const uint8_t zero_kek[sizeof(kek)] = {0};
   assert(memcmp(kek, zero_kek, sizeof(kek)) == 0);
   assert(vault_custody_tpm2_reseal_status(&receipt, "secret", &status) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   assert(status == VAULT_TPM2_RESEAL_ABSENT);
   assert(vault_custody_tpm2_reseal_commit(&receipt, "secret", &status) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   assert(vault_custody_tpm2_reseal_abort(&receipt, "secret") == VAULT_TPM2_RESEAL_NOT_BUILT);
   assert(vault_custody_tpm2_reseal_cleanup(&receipt, "secret",
                                            VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED) ==
          VAULT_TPM2_RESEAL_NOT_BUILT);
   printf("  PASS: test_stub_prepared_reseal_refused\n");
}

/* Bound through the kb policy seam: select tpm2 -> stub bound, live keys stay off. */
static void test_stub_live_keys_off(void)
{
   char err[160] = {0};
   assert(kb_vault_policy_select("tpm2", err, sizeof(err)) == 0);
   assert(vault_is_sealed() == 1);            /* stub reports sealed via the seam */
   assert(kb_vault_live_keys_allowed() == 0); /* fail-closed anchor -> no live keys */

   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) != 0); /* sealed -> no KEK through the facade */

   vault_custody_set_provider(NULL); /* restore file */
   printf("  PASS: test_stub_live_keys_off\n");
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-vault-tpm2-stub-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);

   test_stub_provider_fail_closed();
   test_stub_provision_refused();
   test_stub_prepared_reseal_refused();
   test_stub_live_keys_off();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", home);
   (void)system(rm);
   printf("vault_tpm2_stub: all tests passed\n");
   return 0;
}

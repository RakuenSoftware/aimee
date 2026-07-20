/* test_vault_tpm2.c: WITH_TPM2 integration harness for the tpm2 custody provider
 * (P7-tpm2a). COMPILED ONLY WITH -DWITH_TPM2 (it links libtss2/ESAPI) and driven
 * by scripts/p7_tpm2_swtpm_test.sh against a software TPM2 (swtpm). It is NOT in
 * the default unit-test set (which uses the fail-closed stub via
 * test_vault_tpm2_stub.c) — this file cannot compile without libtss2.
 *
 * A tiny subcommand CLI so the shell script can orchestrate the FIXED test order
 * across FRESH PROCESSES (each invocation re-binds the provider and re-loads the
 * on-disk blob, which is exactly how persistence is exercised):
 *
 *   provision <hexkek> <secret>       seal <hexkek> under <secret> (create-once)
 *   sealed-check                      assert boots SEALED: no KEK, live_keys FALSE
 *   unseal-fail <wrongsecret>         unseal MUST fail + STAY sealed (before success)
 *   unseal-ok <hexkek> <secret>       unseal -> KEK == <hexkek>, live_keys TRUE
 *   seal-after-unseal <hexkek> <secret>  unseal then seal -> sealed again, no KEK
 *   reprovision-refused <hexkek> <secret>  provision MUST refuse (blob exists)
 *   load-fail <secret>                unseal MUST fail closed (truncated/tampered blob)
 *
 * Exit 0 iff the asserted property holds; nonzero (with a diagnostic) otherwise.
 * TCTI + blob path come from AIMEE_VAULT_TPM2_TCTI / AIMEE_VAULT_TPM2_BLOB_PATH
 * (set by the script), so no config file is needed. */
#include "kb/kb_vault_policy.h"
#include "vault_crypto.h"
#include "vault_custody_tpm2.h"
#include "vault_internal.h"
#include "vault_server_key.h"
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int die(const char *msg)
{
   fprintf(stderr, "test_vault_tpm2: FAIL: %s\n", msg);
   return 1;
}

/* Parse exactly VAULT_KEK_LEN*2 hex chars into out[VAULT_KEK_LEN]. 0/-1. */
static int hex_to_kek(const char *hex, uint8_t out[VAULT_KEK_LEN])
{
   if (!hex || strlen(hex) != (size_t)VAULT_KEK_LEN * 2)
      return -1;
   for (int i = 0; i < VAULT_KEK_LEN; i++)
   {
      unsigned int b;
      if (sscanf(hex + i * 2, "%2x", &b) != 1)
         return -1;
      out[i] = (uint8_t)b;
   }
   return 0;
}

/* Bind the tpm2 provider through the kb policy seam (also sets g_selected=tpm2 so
 * kb_vault_live_keys_allowed() reflects the real anchor). 0/-1. */
static int bind_tpm2(void)
{
   char err[192] = {0};
   if (kb_vault_policy_select("tpm2", err, sizeof(err)) != 0)
   {
      fprintf(stderr, "test_vault_tpm2: bind failed: %s\n", err);
      return -1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
      return die("usage: test_vault_tpm2 <command> [args]");
   const char *cmd = argv[1];
   uint8_t kek[VAULT_KEK_LEN];
   int rc = 1;

   if (strcmp(cmd, "provision") == 0)
   {
      if (argc != 4 || hex_to_kek(argv[2], kek) != 0)
         return die("provision <hexkek(64)> <secret>");
      char err[192] = {0};
      int pr = vault_custody_tpm2_provision(kek, argv[3], err, sizeof(err));
      OPENSSL_cleanse(kek, sizeof(kek));
      if (pr != 0)
      {
         fprintf(stderr, "test_vault_tpm2: provision error: %s\n", err);
         return 1;
      }
      printf("test_vault_tpm2: provision OK\n");
      return 0;
   }

   if (bind_tpm2() != 0)
      return 1;

   if (strcmp(cmd, "sealed-check") == 0)
   {
      /* (a) boots SEALED: no KEK, live_keys FALSE. */
      if (vault_is_sealed() != 1)
         return die("expected sealed after bind");
      if (vault_server_kek(kek) == 0)
         return die("get_kek succeeded while sealed");
      if (kb_vault_live_keys_allowed() != 0)
         return die("live_keys TRUE while sealed");
      printf("test_vault_tpm2: sealed-check OK\n");
      rc = 0;
   }
   else if (strcmp(cmd, "unseal-fail") == 0)
   {
      /* (b) WRONG secret -> refused, STAYS sealed (run BEFORE any success). */
      if (argc != 3)
         return die("unseal-fail <wrongsecret>");
      if (vault_unseal(argv[2], strlen(argv[2])) == 0)
         return die("unseal SUCCEEDED with wrong secret");
      if (vault_is_sealed() != 1)
         return die("provider not sealed after failed unseal");
      if (kb_vault_live_keys_allowed() != 0)
         return die("live_keys TRUE after failed unseal");
      printf("test_vault_tpm2: unseal-fail (correctly refused) OK\n");
      rc = 0;
   }
   else if (strcmp(cmd, "unseal-ok") == 0)
   {
      /* (c)/(e) correct secret -> exact KEK, live_keys TRUE. A fresh process here
       * proves on-disk-blob persistence (re-load under the primary). */
      uint8_t want[VAULT_KEK_LEN];
      if (argc != 4 || hex_to_kek(argv[2], want) != 0)
         return die("unseal-ok <hexkek(64)> <secret>");
      if (vault_unseal(argv[3], strlen(argv[3])) != 0)
         return die("unseal failed with correct secret");
      if (vault_is_sealed() != 0)
         return die("still sealed after successful unseal");
      if (vault_server_kek(kek) != 0)
         return die("get_kek failed after unseal");
      if (memcmp(kek, want, VAULT_KEK_LEN) != 0)
         return die("unsealed KEK != provisioned KEK");
      if (kb_vault_live_keys_allowed() != 1)
         return die("live_keys not TRUE after unseal on real anchor");
      OPENSSL_cleanse(kek, sizeof(kek));
      OPENSSL_cleanse(want, sizeof(want));
      printf("test_vault_tpm2: unseal-ok (KEK matches, live_keys TRUE) OK\n");
      rc = 0;
   }
   else if (strcmp(cmd, "seal-after-unseal") == 0)
   {
      /* (d) unseal then seal -> sealed again, no KEK, live_keys FALSE. */
      if (argc != 4)
         return die("seal-after-unseal <hexkek(64)> <secret>");
      if (vault_unseal(argv[3], strlen(argv[3])) != 0)
         return die("unseal failed with correct secret");
      if (vault_is_sealed() != 0)
         return die("still sealed after unseal");
      if (vault_seal() != 0)
         return die("seal returned error");
      if (vault_is_sealed() != 1)
         return die("not sealed after seal()");
      if (vault_server_kek(kek) == 0)
         return die("get_kek succeeded after seal");
      if (kb_vault_live_keys_allowed() != 0)
         return die("live_keys TRUE after seal");
      printf("test_vault_tpm2: seal-after-unseal OK\n");
      rc = 0;
   }
   else if (strcmp(cmd, "reprovision-refused") == 0)
   {
      /* (g) re-provision while a blob exists -> REFUSED (create-once). */
      if (argc != 4 || hex_to_kek(argv[2], kek) != 0)
         return die("reprovision-refused <hexkek(64)> <secret>");
      char err[192] = {0};
      int pr = vault_custody_tpm2_provision(kek, argv[3], err, sizeof(err));
      OPENSSL_cleanse(kek, sizeof(kek));
      if (pr == 0)
         return die("re-provision SUCCEEDED (create-once violated)");
      printf("test_vault_tpm2: reprovision-refused (correctly refused: %s) OK\n", err);
      rc = 0;
   }
   else if (strcmp(cmd, "load-fail") == 0)
   {
      /* (h) truncated/tampered blob -> unseal fails closed, stays sealed. */
      if (argc != 3)
         return die("load-fail <secret>");
      if (vault_unseal(argv[2], strlen(argv[2])) == 0)
         return die("unseal SUCCEEDED on a truncated/tampered blob");
      if (vault_is_sealed() != 1)
         return die("not sealed after failed load");
      printf("test_vault_tpm2: load-fail (correctly failed closed) OK\n");
      rc = 0;
   }
   else
   {
      return die("unknown command");
   }

   OPENSSL_cleanse(kek, sizeof(kek));
   return rc;
}

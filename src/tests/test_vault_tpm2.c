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
 * P7-tpm2b (anti-rollback) subcommands:
 *   nv-read <secret>                  print the current NV counter value (defence check)
 *   reseal <newhexkek> <secret>       reseal to a NEW generation (NV++), new blob binds G'
 *   unseal-refused <secret>           unseal MUST fail (stale blob refused BY the TPM/PolicyNV)
 *   reseal-fail <newhexkek> <wrongsecret>  reseal MUST fail (wrong secret can't NV++)
 *   craft-v1-blob                     write a v1 (tpm2a-magic) blob to the blob path
 *   v1-refused <secret>               unseal MUST refuse a v1 blob (re-provision to v2)
 *
 * Exit 0 iff the asserted property holds; nonzero (with a diagnostic) otherwise.
 * TCTI + blob path come from AIMEE_VAULT_TPM2_TCTI / AIMEE_VAULT_TPM2_BLOB_PATH
 * (set by the script); the NV index from AIMEE_VAULT_TPM2_NV_INDEX (optional), so no
 * config file is needed. */
#include "kb/kb_vault_policy.h"
#include "vault_crypto.h"
#include "vault_custody_tpm2.h"
#include "vault_internal.h"
#include "vault_reseal_receipt.h"
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

static int receipt_path(char *out, size_t cap)
{
   const char *blob = getenv("AIMEE_VAULT_TPM2_BLOB_PATH");
   return blob && (size_t)snprintf(out, cap, "%s.test-receipt", blob) < cap ? 0 : -1;
}

static int receipt_save(const vault_tpm2_reseal_receipt_t *r)
{
   char path[1200];
   if (receipt_path(path, sizeof(path)) != 0)
      return -1;
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   if (vault_reseal_receipt_encode(r, wire) != 0)
      return -1;
   FILE *f = fopen(path, "wb");
   int ok = f && fwrite(wire, 1, sizeof(wire), f) == sizeof(wire) && fflush(f) == 0;
   if (f)
      fclose(f);
   OPENSSL_cleanse(wire, sizeof(wire));
   return ok ? 0 : -1;
}

static int receipt_load(vault_tpm2_reseal_receipt_t *r)
{
   if (!r)
      return -1;
   OPENSSL_cleanse(r, sizeof(*r));
   char path[1200];
   if (receipt_path(path, sizeof(path)) != 0)
      return -1;
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN + 1];
   FILE *f = fopen(path, "rb");
   size_t n = f ? fread(wire, 1, sizeof(wire), f) : 0;
   int ok = f && n == VAULT_RESEAL_RECEIPT_V1_LEN && fgetc(f) == EOF &&
            vault_reseal_receipt_decode(wire, n, r) == 0;
   if (f)
      fclose(f);
   OPENSSL_cleanse(wire, sizeof(wire));
   return ok ? 0 : -1;
}

static const uint8_t prepared_operation_id[16] = {0x50, 0x37, 0x52, 0x53, 0x41, 0x2d, 0x54, 0x45,
                                                  0x53, 0x54, 0x2d, 0x4f, 0x50, 0x2d, 0x30, 0x31};

static int bytes_zero(const uint8_t *p, size_t n)
{
   uint8_t v = 0;
   for (size_t i = 0; i < n; i++)
      v |= p[i];
   return v == 0;
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

   if (strcmp(cmd, "reseal") == 0)
   {
      /* (j)/(l) reseal to a NEW generation (NV++); the new blob binds G'. */
      if (argc != 4 || hex_to_kek(argv[2], kek) != 0)
         return die("reseal <newhexkek(64)> <secret>");
      int pr = vault_custody_tpm2_reseal(kek, argv[3]);
      OPENSSL_cleanse(kek, sizeof(kek));
      if (pr != 0)
         return die("reseal failed with correct secret");
      printf("test_vault_tpm2: reseal OK\n");
      return 0;
   }

   if (strcmp(cmd, "reseal-fail") == 0)
   {
      /* (m) a reseal (which must NV_Increment) under a WRONG secret MUST fail: the
       * NV AUTHWRITE is gated by the secret-derived authValue, so a wrong secret
       * cannot bump the counter. */
      if (argc != 4 || hex_to_kek(argv[2], kek) != 0)
         return die("reseal-fail <newhexkek(64)> <wrongsecret>");
      int pr = vault_custody_tpm2_reseal(kek, argv[3]);
      OPENSSL_cleanse(kek, sizeof(kek));
      if (pr == 0)
         return die("reseal SUCCEEDED with wrong secret (NV auth not enforced)");
      printf("test_vault_tpm2: reseal-fail (wrong secret correctly refused) OK\n");
      return 0;
   }

   if (strcmp(cmd, "prepared-prepare") == 0 || strcmp(cmd, "prepared-prepare-drop-receipt") == 0)
   {
      if (argc != 4 || hex_to_kek(argv[2], kek) != 0)
         return die("prepared-prepare[-drop-receipt] <newhexkek(64)> <secret>");
      uint64_t gen = 0;
      vault_tpm2_reseal_receipt_t receipt;
      int pr = vault_custody_tpm2_nv_generation(argv[3], &gen);
      if (pr == 0)
         pr = vault_custody_tpm2_reseal_prepare(prepared_operation_id, gen, kek, argv[3], &receipt);
      OPENSSL_cleanse(kek, sizeof(kek));
      int drop = strcmp(cmd, "prepared-prepare-drop-receipt") == 0;
      if (pr != VAULT_TPM2_RESEAL_OK || (!drop && receipt_save(&receipt) != 0))
         return die("prepared prepare failed");
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      printf("test_vault_tpm2: prepared-prepare OK gen=%llu\n", (unsigned long long)gen);
      return 0;
   }

   if (strcmp(cmd, "prepared-discover") == 0 || strcmp(cmd, "prepared-discover-absent") == 0 ||
       strcmp(cmd, "prepared-discover-corrupt") == 0 ||
       strcmp(cmd, "prepared-discover-busy") == 0 ||
       strcmp(cmd, "prepared-discover-wrong-op") == 0 ||
       strcmp(cmd, "prepared-discover-wrong-generation") == 0 ||
       strcmp(cmd, "prepared-discover-wrong-secret") == 0)
   {
      if (argc != 3)
         return die("prepared-discover* <secret>");
      uint64_t gen = 0;
      if (strcmp(cmd, "prepared-discover-wrong-secret") == 0)
      {
         vault_tpm2_reseal_receipt_t saved;
         if (receipt_load(&saved) != 0)
            return die("prepared receipt missing for wrong-secret discovery");
         gen = saved.old_generation;
         OPENSSL_cleanse(&saved, sizeof(saved));
      }
      else if (vault_custody_tpm2_nv_generation(argv[2], &gen) != 0)
         return die("prepared discover NV read failed");
      uint8_t op[16];
      memcpy(op, prepared_operation_id, sizeof(op));
      if (strcmp(cmd, "prepared-discover-wrong-op") == 0)
         op[0] ^= 0x80;
      if (strcmp(cmd, "prepared-discover-wrong-generation") == 0)
         gen++;
      vault_tpm2_reseal_receipt_t receipt;
      memset(&receipt, 0x5a, sizeof(receipt));
      vault_tpm2_reseal_status_t status = VAULT_TPM2_RESEAL_CORRUPT;
      int pr = vault_custody_tpm2_reseal_discover(op, gen, argv[2], &receipt, &status);
      int want_ok = strcmp(cmd, "prepared-discover") == 0;
      int want_absent = strcmp(cmd, "prepared-discover-absent") == 0;
      int want_busy = strcmp(cmd, "prepared-discover-busy") == 0;
      int want_corrupt = strcmp(cmd, "prepared-discover-corrupt") == 0;
      int want_conflict = strcmp(cmd, "prepared-discover-wrong-op") == 0 ||
                          strcmp(cmd, "prepared-discover-wrong-generation") == 0;
      if ((want_ok && (pr != VAULT_TPM2_RESEAL_OK || status != VAULT_TPM2_RESEAL_PREPARED ||
                       bytes_zero((const uint8_t *)&receipt, sizeof(receipt)) ||
                       receipt_save(&receipt) != 0)) ||
          (want_absent && (pr != VAULT_TPM2_RESEAL_OK || status != VAULT_TPM2_RESEAL_ABSENT ||
                           !bytes_zero((const uint8_t *)&receipt, sizeof(receipt)))) ||
          (want_busy && pr != VAULT_TPM2_RESEAL_BUSY) ||
          (want_corrupt &&
           (pr != VAULT_TPM2_RESEAL_INTEGRITY || status != VAULT_TPM2_RESEAL_CORRUPT)) ||
          (want_conflict &&
           (pr != VAULT_TPM2_RESEAL_INTEGRITY || status != VAULT_TPM2_RESEAL_CONFLICT)) ||
          (!want_ok && !want_absent &&
           (pr == VAULT_TPM2_RESEAL_OK || !bytes_zero((const uint8_t *)&receipt, sizeof(receipt)))))
      {
         fprintf(stderr,
                 "test_vault_tpm2: prepared discover mismatch command=%s result=%d status=%d "
                 "receipt_zero=%d\n",
                 cmd, pr, (int)status, bytes_zero((const uint8_t *)&receipt, sizeof(receipt)));
         return die("prepared discover classification/output mismatch");
      }
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      printf("test_vault_tpm2: %s OK status=%d\n", cmd, (int)status);
      return 0;
   }

   if (strcmp(cmd, "prepared-recover") == 0 || strcmp(cmd, "prepared-recover-refused") == 0)
   {
      int expect_ok = strcmp(cmd, "prepared-recover") == 0;
      if ((expect_ok && (argc != 4 || hex_to_kek(argv[2], kek) != 0)) ||
          (!expect_ok && argc != 3 && argc != 4))
         return die("prepared-recover <expectedhexkek> <secret> | prepared-recover-refused "
                    "<secret> [receipt-field]");
      vault_tpm2_reseal_receipt_t receipt;
      if (receipt_load(&receipt) != 0)
         return die("prepared receipt missing");
      if (!expect_ok && argc == 4)
      {
         if (strcmp(argv[3], "operation") == 0)
            receipt.operation_id[0] ^= 0x80;
         else if (strcmp(argv[3], "old-generation") == 0)
            receipt.old_generation++;
         else if (strcmp(argv[3], "new-generation") == 0)
            receipt.new_generation++;
         else if (strcmp(argv[3], "predecessor") == 0)
            receipt.predecessor_digest[0] ^= 0x80;
         else if (strcmp(argv[3], "capsule") == 0)
            receipt.capsule_digest[0] ^= 0x80;
         else if (strcmp(argv[3], "future") == 0)
            receipt.future_digest[0] ^= 0x80;
         else if (strcmp(argv[3], "new-kek") == 0)
            receipt.new_kek_digest[0] ^= 0x80;
         else if (strcmp(argv[3], "manifest") == 0)
            receipt.manifest_digest[0] ^= 0x80;
         else
            return die("unknown receipt field mutation");
      }
      uint8_t recovered[VAULT_KEK_LEN];
      memset(recovered, 0x5a, sizeof(recovered));
      const char *secret = expect_ok ? argv[3] : argv[2];
      int pr = vault_custody_tpm2_reseal_recover_kek(&receipt, secret, recovered);
      if ((expect_ok &&
           (pr != VAULT_TPM2_RESEAL_OK || CRYPTO_memcmp(recovered, kek, sizeof(kek)) != 0)) ||
          (!expect_ok && (pr == VAULT_TPM2_RESEAL_OK || !bytes_zero(recovered, sizeof(recovered)))))
         return die("prepared recover result/output mismatch");
      const vault_custody_provider_t *provider = vault_custody_tpm2_provider();
      uint8_t unavailable[VAULT_KEK_LEN];
      memset(unavailable, 0x5a, sizeof(unavailable));
      if (!provider || provider->is_sealed(provider->ctx) != 1 ||
          provider->get_kek(provider->ctx, unavailable) == 0 ||
          !bytes_zero(unavailable, sizeof(unavailable)))
         return die("prepared recovery changed sealed provider/cache state");
      OPENSSL_cleanse(recovered, sizeof(recovered));
      OPENSSL_cleanse(unavailable, sizeof(unavailable));
      OPENSSL_cleanse(kek, sizeof(kek));
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      printf("test_vault_tpm2: %s OK\n", cmd);
      return 0;
   }

   if (strcmp(cmd, "prepared-status") == 0 || strcmp(cmd, "prepared-commit") == 0 ||
       strcmp(cmd, "prepared-cleanup") == 0 || strcmp(cmd, "prepared-abort") == 0)
   {
      if (argc != 3)
         return die("prepared-{status,commit,cleanup,abort} <secret>");
      vault_tpm2_reseal_receipt_t receipt;
      vault_tpm2_reseal_status_t status = VAULT_TPM2_RESEAL_CORRUPT;
      if (receipt_load(&receipt) != 0)
         return die("prepared receipt missing");
      int pr;
      if (strcmp(cmd, "prepared-status") == 0)
         pr = vault_custody_tpm2_reseal_status(&receipt, argv[2], &status);
      else if (strcmp(cmd, "prepared-commit") == 0)
         pr = vault_custody_tpm2_reseal_commit(&receipt, argv[2], &status);
      else if (strcmp(cmd, "prepared-abort") == 0)
         pr = vault_custody_tpm2_reseal_abort(&receipt, argv[2]);
      else
         pr = vault_custody_tpm2_reseal_cleanup(&receipt, argv[2],
                                                VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED);
      if (pr != VAULT_TPM2_RESEAL_OK)
         return die("prepared operation failed");
      printf("test_vault_tpm2: %s OK status=%d\n", cmd, (int)status);
      return 0;
   }

   if (strcmp(cmd, "nv-read") == 0)
   {
      /* (i)/(l) print the current NV monotonic-counter generation. */
      if (argc != 3)
         return die("nv-read <secret>");
      uint64_t gen = 0;
      if (vault_custody_tpm2_nv_generation(argv[2], &gen) != 0)
         return die("nv-read failed");
      printf("test_vault_tpm2: nv-read OK gen=%llu\n", (unsigned long long)gen);
      return 0;
   }

   if (strcmp(cmd, "craft-v1-blob") == 0)
   {
      /* (k) write a v1 (tpm2a-magic "AIMTPM2\\0") blob to the blob path so tpm2b
       * unseal must refuse it. blob_read discriminates v1 by the 8-byte magic alone,
       * so a minimal v1 header suffices to exercise the refusal path. */
      const char *path = getenv("AIMEE_VAULT_TPM2_BLOB_PATH");
      if (!path || !path[0])
         return die("craft-v1-blob needs AIMEE_VAULT_TPM2_BLOB_PATH");
      const unsigned char v1[16] = {'A', 'I', 'M', 'T', 'P', 'M', '2', 0, 0, 0, 0, 0, 0, 0, 0, 0};
      FILE *f = fopen(path, "wb");
      if (!f || fwrite(v1, 1, sizeof(v1), f) != sizeof(v1))
      {
         if (f)
            fclose(f);
         return die("craft-v1-blob: write failed");
      }
      fclose(f);
      printf("test_vault_tpm2: craft-v1-blob OK\n");
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
   else if (strcmp(cmd, "unseal-refused") == 0)
   {
      /* (j)/(l) a STALE blob (an old generation restored after a reseal bumped the NV
       * counter) MUST be refused: its PolicyNV asserts NV == old-gen, now false, so
       * Esys_Unseal fails AT THE TPM. The provider stays sealed. This is TPM-enforced
       * anti-rollback — the refusal comes from PolicyNV, not our software gen-check. */
      if (argc != 3)
         return die("unseal-refused <secret>");
      if (vault_unseal(argv[2], strlen(argv[2])) == 0)
         return die("unseal SUCCEEDED on a stale blob (PolicyNV anti-rollback FAILED)");
      if (vault_is_sealed() != 1)
         return die("provider not sealed after refused stale unseal");
      if (kb_vault_live_keys_allowed() != 0)
         return die("live_keys TRUE after refused stale unseal");
      printf("test_vault_tpm2: unseal-refused (stale blob refused by TPM/PolicyNV) OK\n");
      rc = 0;
   }
   else if (strcmp(cmd, "v1-refused") == 0)
   {
      /* (k) a v1 (tpm2a, generation-less) blob MUST be refused — tpm2b requires a
       * re-provision to v2 (accepting a v1 blob would reopen the rollback hole). */
      if (argc != 3)
         return die("v1-refused <secret>");
      if (vault_unseal(argv[2], strlen(argv[2])) == 0)
         return die("unseal SUCCEEDED on a v1 blob (must require re-provision to v2)");
      if (vault_is_sealed() != 1)
         return die("provider not sealed after refused v1 unseal");
      printf("test_vault_tpm2: v1-refused (v1 blob refused; re-provision to v2) OK\n");
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

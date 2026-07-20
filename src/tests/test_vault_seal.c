/* test_vault_seal.c: P10/P7 slice 3b — the custody seal/unseal barrier + the kb
 * §3 live-key gate.
 *
 * Pins:
 *   (1) file custody (default/server profile) is ALWAYS unsealed: vault_is_sealed()
 *       == 0 and vault_server_kek() works.
 *   (2) the mock anchor is a real seal state machine: fresh -> SEALED (get_kek
 *       fails), unseal(secret) -> UNSEALED with a stable KEK, seal() -> re-sealed +
 *       KEK zeroized + the process KEK cache flushed, re-unseal -> works again.
 *   (3) vault_seal() flushes the process-wide KEK cache.
 *   (4) kb_vault_live_keys_allowed() is FALSE for file AND for mock (even unsealed)
 *       — a live key requires a REAL anchor (tpm2/pkcs11/kms), which are unimplemented.
 *   (5) kb_vault_policy_select accepts {file,mock}, rejects typos, and fails closed
 *       for the unimplemented anchors. */
#include "kb/kb_vault_policy.h"
#include "vault_crypto.h"
#include "vault_custody_mock.h"
#include "vault_internal.h"
#include "vault_kek_cache.h"
#include "vault_server_key.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[256];

/* ── (1) file custody: always unsealed, server KEK works ─────────────────────── */
static void test_file_custody_always_unsealed(void)
{
   vault_custody_set_provider(NULL); /* the built-in file provider */
   assert(vault_is_sealed() == 0);   /* file never seals */

   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) == 0); /* self-unsealing: KEK available with no unseal */

   /* unseal/seal are no-ops under file (NULL provider slots) — never error. */
   assert(vault_unseal("ignored", 7) == 0);
   assert(vault_is_sealed() == 0);
   assert(vault_seal() == 0); /* no-op seal (still flushes the cache) */
   assert(vault_is_sealed() == 0);
   assert(vault_server_kek(kek) == 0); /* still works after a no-op seal */
   OPENSSL_cleanse(kek, sizeof(kek));
   printf("  PASS: test_file_custody_always_unsealed\n");
}

/* ── (2)+(3) mock anchor seal state machine, driven through the seam ─────────── */
static void test_mock_seal_state_machine(void)
{
   vault_custody_mock_reset();
   vault_custody_set_provider(vault_custody_mock_provider());

   /* Fresh mock is SEALED: get_kek fails. */
   assert(vault_is_sealed() == 1);
   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) != 0); /* sealed anchor yields no KEK */

   /* Unseal -> UNSEALED, get_kek returns a stable KEK. */
   const char *secret = "correct horse battery staple";
   assert(vault_unseal(secret, strlen(secret)) == 0);
   assert(vault_is_sealed() == 0);
   uint8_t kek_a[VAULT_KEK_LEN], kek_b[VAULT_KEK_LEN];
   assert(vault_server_kek(kek_a) == 0);
   assert(vault_server_kek(kek_b) == 0);
   assert(memcmp(kek_a, kek_b, VAULT_KEK_LEN) == 0); /* stable across calls */

   /* Seed the process KEK cache, then seal: cache MUST be flushed + get_kek fails. */
   uint8_t dummy[VAULT_KEK_LEN];
   memset(dummy, 0x42, sizeof(dummy));
   assert(vault_kek_cache_put("uid:seal-test", dummy, 1000) == 0);
   assert(vault_kek_cache_count(1000) >= 1);
   assert(vault_seal() == 0);
   assert(vault_kek_cache_count(1000) == 0); /* (3) seal flushed the KEK cache */
   assert(vault_is_sealed() == 1);
   assert(vault_server_kek(kek) != 0); /* re-sealed: no KEK until re-unseal */

   /* Re-unseal with the same secret re-derives the same KEK. */
   assert(vault_unseal(secret, strlen(secret)) == 0);
   assert(vault_is_sealed() == 0);
   uint8_t kek_c[VAULT_KEK_LEN];
   assert(vault_server_kek(kek_c) == 0);
   assert(memcmp(kek_a, kek_c, VAULT_KEK_LEN) == 0); /* deterministic re-derive */

   /* A different secret derives a different KEK. */
   vault_custody_mock_reset();
   assert(vault_unseal("a different secret", 18) == 0);
   uint8_t kek_d[VAULT_KEK_LEN];
   assert(vault_server_kek(kek_d) == 0);
   assert(memcmp(kek_a, kek_d, VAULT_KEK_LEN) != 0);

   OPENSSL_cleanse(kek_a, sizeof(kek_a));
   OPENSSL_cleanse(kek_b, sizeof(kek_b));
   OPENSSL_cleanse(kek_c, sizeof(kek_c));
   OPENSSL_cleanse(kek_d, sizeof(kek_d));
   vault_custody_set_provider(NULL); /* restore file */
   printf("  PASS: test_mock_seal_state_machine\n");
}

/* ── (4) the §3 live-key gate: false for file AND mock (even unsealed) ───────── */
static void test_live_keys_gate(void)
{
   char err[160];

   /* file -> not a real anchor -> false. */
   assert(kb_vault_policy_select("file", err, sizeof(err)) == 0);
   assert(kb_vault_live_keys_allowed() == 0);

   /* mock -> test/dev only, NEVER live-key-eligible even once unsealed. */
   vault_custody_mock_reset();
   assert(kb_vault_policy_select("mock", err, sizeof(err)) == 0);
   assert(kb_vault_live_keys_allowed() == 0); /* sealed mock */
   assert(vault_unseal("s", 1) == 0);
   assert(vault_is_sealed() == 0);
   assert(kb_vault_live_keys_allowed() == 0); /* UNSEALED mock still excluded (security-critical) */

   vault_custody_mock_reset();
   vault_custody_set_provider(NULL);
   printf("  PASS: test_live_keys_gate\n");
}

/* ── (5) config selection: valid accepted, typo rejected, anchors fail closed ── */
static void test_policy_selection(void)
{
   char err[160];

   assert(kb_vault_policy_select("file", err, sizeof(err)) == 0);
   assert(kb_vault_policy_select(NULL, err, sizeof(err)) == 0);  /* NULL -> file */
   assert(kb_vault_policy_select("", err, sizeof(err)) == 0);    /* empty -> file */
   assert(kb_vault_policy_select("mock", err, sizeof(err)) == 0);

   /* Unimplemented real anchors: parse OK but FAIL CLOSED at bind. */
   err[0] = '\0';
   assert(kb_vault_policy_select("tpm2", err, sizeof(err)) != 0);
   assert(strstr(err, "not yet implemented") != NULL);
   assert(kb_vault_policy_select("pkcs11", err, sizeof(err)) != 0);
   assert(kb_vault_policy_select("kms", err, sizeof(err)) != 0);

   /* Typos / unknown values are rejected (validation). */
   err[0] = '\0';
   assert(kb_vault_policy_select("tpm", err, sizeof(err)) != 0);
   assert(strstr(err, "not a known value") != NULL);
   assert(kb_vault_policy_select("TPM2", err, sizeof(err)) != 0);
   assert(kb_vault_policy_select("kmss", err, sizeof(err)) != 0);
   assert(kb_vault_policy_select("filee", err, sizeof(err)) != 0);

   vault_custody_mock_reset();
   vault_custody_set_provider(NULL);
   printf("  PASS: test_policy_selection\n");
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vault-seal-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);

   test_file_custody_always_unsealed();
   test_mock_seal_state_machine();
   test_live_keys_gate();
   test_policy_selection();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_seal: all tests passed\n");
   return 0;
}

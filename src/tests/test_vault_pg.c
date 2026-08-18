/* test_vault_pg.c: the Postgres credential-vault backend (P10 slice 2), exercised
 * through the PUBLIC vault_store_* facade after binding vault_pg_backend.
 *
 * REAL-PG ONLY: RLS + the SECURITY DEFINER envelope machinery are Postgres controls
 * the SQLite shim cannot emulate, so this test requires a live Postgres. It reads the
 * connection URL from AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) when it is unset —
 * mirroring the p1/p3a RLS gate posture (the gate itself does not skip; this unit test
 * does, so `make unit-tests` on a box without Postgres stays green). Validated for real
 * on CT103.
 *
 * Proves: (1) envelope round-trip through the pg backend (set -> get returns the
 * secret); (2) a wrong KEK is rejected (fail-closed, not a silent downgrade); (3) the
 * version pointer starts at 1; (4) AAD binding — a credential set under one slot does
 * not decrypt as another. Uses a PLATFORM-scoped principal (team_id NULL) so the test
 * is self-contained (no kb_team seed needed); team-scoped RLS isolation is proven in
 * scripts/p10_vault_rls_test.sql. */
#include "vault_store.h"
#include "vault_crypto.h"
#include "vault_kek_check.h"
#include "vault_internal.h"             /* vault_store_set_backend */
#include "modules/db2/c/vault_pg.h"     /* vault_pg_backend */
#include "modules/db2/c/db2.h"          /* db2_init / db2_shutdown */
#include "modules/db2/c/db2_internal.h" /* db2_conn (direct version-pointer assertion) */
#include "modules/db2/c/db_postgres.h"  /* aimee_pg_* */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void make_kek(uint8_t kek[VAULT_KEK_LEN], unsigned seed)
{
   for (int i = 0; i < VAULT_KEK_LEN; i++)
      kek[i] = (uint8_t)(seed * 31 + i * 7 + 1);
}

static int invalid_random(uint8_t *out, size_t len)
{
   memset(out, 0xa5, len);
   return 1;
}

static void register_crypto_provider(void)
{
   const db2_vault_crypto_provider_t provider = {
       .aad_build_v2 = vault_aad_build_v2,
       .aad_build_v1_safe = vault_aad_build_v1_safe,
       .random = vault_crypto_random,
       .dek_wrap = vault_dek_wrap,
       .dek_unwrap = vault_dek_unwrap,
       .secret_encrypt = vault_secret_encrypt,
       .secret_decrypt = vault_secret_decrypt,
       .kek_check_wrap = vault_kek_check_wrap,
       .kek_check_verify = vault_kek_check_verify,
   };
   aimee_db2_register_vault_crypto_provider(&provider);
}

static void test_crypto_contract(void)
{
   uint8_t random_bytes[VAULT_SALT_LEN];
   memset(random_bytes, 0xa5, sizeof(random_bytes));
   aimee_db2_register_vault_crypto_provider(NULL);
   assert(db2_vault_crypto_random(random_bytes, sizeof(random_bytes)) == -1);
   for (size_t i = 0; i < sizeof(random_bytes); ++i)
      assert(random_bytes[i] == 0);

   db2_vault_crypto_provider_t invalid = {.random = invalid_random};
   aimee_db2_register_vault_crypto_provider(&invalid);
   memset(random_bytes, 0xa5, sizeof(random_bytes));
   assert(db2_vault_crypto_random(random_bytes, sizeof(random_bytes)) == -1);
   for (size_t i = 0; i < sizeof(random_bytes); ++i)
      assert(random_bytes[i] == 0);

   register_crypto_provider();
   assert(db2_vault_crypto_random(random_bytes, sizeof(random_bytes)) == 0);

   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   assert(db2_vault_aad_build_v2("owner", "agent", "credential", 1, aad, sizeof(aad), &aad_len) ==
          0);
   assert(aad_len > 0);
   assert(db2_vault_aad_build_v1_safe("owner", "agent", "credential", 1, aad, sizeof(aad),
                                      &aad_len) == 0);

   uint8_t kek[VAULT_KEK_LEN], wrong[VAULT_KEK_LEN], dek[VAULT_DEK_LEN], unwrapped[VAULT_DEK_LEN];
   make_kek(kek, 1);
   make_kek(wrong, 2);
   memset(dek, 0x42, sizeof(dek));
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   assert(db2_vault_dek_wrap(kek, dek, wrapped) == 0);
   assert(db2_vault_dek_unwrap(kek, wrapped, unwrapped) == 0);
   assert(memcmp(dek, unwrapped, sizeof(dek)) == 0);

   const uint8_t plaintext[] = "contract-secret";
   uint8_t nonce[VAULT_GCM_NONCE_LEN], ciphertext[sizeof(plaintext)];
   uint8_t tag[VAULT_GCM_TAG_LEN], decrypted[sizeof(plaintext)];
   assert(db2_vault_secret_encrypt(dek, aad, aad_len, plaintext, sizeof(plaintext), nonce,
                                   ciphertext, tag) == 0);
   assert(db2_vault_secret_decrypt(dek, aad, aad_len, nonce, ciphertext, sizeof(ciphertext), tag,
                                   decrypted) == 0);
   assert(memcmp(plaintext, decrypted, sizeof(plaintext)) == 0);

   assert(db2_vault_kek_check_wrap(kek, wrapped) == 0);
   assert(db2_vault_kek_check_verify(kek, wrapped) == 0);
   assert(db2_vault_kek_check_verify(wrong, wrapped) == -1);
}

/* Read the current version of a slot straight from org_vault_has, so we can assert the
 * pointer starts at 1 (not just that a round-trip is self-consistent). */
static int64_t slot_version(const char *principal, const char *agent, const char *cred)
{
   void *conn = db2_conn();
   assert(conn);
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_vault_has(?1, ?2, ?3)", err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_bind_text(st, "?2", agent);
   aimee_pg_bind_text(st, "?3", cred);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int64_t v = (rc == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : -1;
   aimee_pg_finalize(st);
   return v;
}

static void run(void)
{
   const char *principal = "org:test:vault-pg"; /* platform-scoped (team_id NULL) */
   const char *agent = "claude";
   const char *cred = "api_key";
   const char *secret = "sk-pg-envelope-roundtrip-0xC0FFEE";

   uint8_t kek[VAULT_KEK_LEN], wrong[VAULT_KEK_LEN];
   make_kek(kek, 5);
   make_kek(wrong, 6);

   /* Fresh slate for a re-run against the same db. */
   (void)vault_store_delete(principal, agent, cred);

   /* Salt + verifier: establish, then a correct/incorrect KEK is caught by unlock. */
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(principal, salt) == 0);
   assert(vault_store_unlock_check(principal, kek) == 0);   /* first unlock: establish */
   assert(vault_store_unlock_check(principal, kek) == 0);   /* matches thereafter */
   assert(vault_store_unlock_check(principal, wrong) != 0); /* wrong KEK rejected */
   printf("  PASS: salt + kek_check verifier\n");

   /* (1) envelope round-trip + (3) version pointer starts at 1. */
   assert(vault_store_set(principal, kek, agent, cred, secret) == 0);
   assert(slot_version(principal, agent, cred) == 1);
   char out[256] = {0};
   assert(vault_store_get(principal, kek, agent, cred, out, sizeof(out)) == 0);
   assert(strcmp(out, secret) == 0);
   assert(vault_store_has_entry(principal, agent, cred) == 1);
   printf("  PASS: envelope round-trip + version pointer starts at 1\n");

   /* (2) wrong KEK on read is fail-closed (-1), not NO_ENTRY, and clears out. */
   memset(out, 'x', sizeof(out));
   assert(vault_store_get(principal, wrong, agent, cred, out, sizeof(out)) == -1);
   assert(out[0] == '\0');
   printf("  PASS: wrong-KEK read rejected (fail-closed)\n");

   /* (4) AAD binding: a different slot (agent) is NO_ENTRY, and once set under its own
    * AAD, decrypting it as the first slot's identity is impossible (they never collide). */
   const char *agent2 = "codex";
   assert(vault_store_get(principal, kek, agent2, cred, out, sizeof(out)) == VAULT_STORE_NO_ENTRY);
   assert(vault_store_set(principal, kek, agent2, cred, "sk-second-slot") == 0);
   assert(vault_store_get(principal, kek, agent2, cred, out, sizeof(out)) == 0);
   assert(strcmp(out, "sk-second-slot") == 0);
   /* First slot still returns ITS secret (AAD keeps the two identities distinct). */
   assert(vault_store_get(principal, kek, agent, cred, out, sizeof(out)) == 0);
   assert(strcmp(out, secret) == 0);
   printf("  PASS: AAD binds identity slot (no cross-slot decrypt)\n");

   /* list sees both slots; delete removes one; the deleted read is NO_ENTRY. */
   vault_store_entry_t entries[8];
   int n = vault_store_list(principal, entries, 8);
   assert(n == 2);
   assert(vault_store_delete(principal, agent2, cred) == 0);
   assert(vault_store_get(principal, kek, agent2, cred, out, sizeof(out)) == VAULT_STORE_NO_ENTRY);
   assert(vault_store_has_entry(principal, agent, cred) == 1);
   printf("  PASS: list / delete\n");

   /* rekey: re-wrap under a new KEK; the secret is still readable under new, not old. */
   uint8_t newkek[VAULT_KEK_LEN];
   make_kek(newkek, 9);
   assert(vault_store_rekey(principal, kek, newkek) == 0);
   assert(vault_store_get(principal, newkek, agent, cred, out, sizeof(out)) == 0);
   assert(strcmp(out, secret) == 0);
   assert(vault_store_get(principal, kek, agent, cred, out, sizeof(out)) == -1); /* old KEK dead */
   printf("  PASS: rekey re-wraps DEKs (secret intact, old KEK rejected)\n");

   /* The server-autonomous dual-wrap ops are unsupported on this backend (return -1). */
   assert(vault_store_set_server(principal, newkek, agent, cred, "x") == -1);
   assert(vault_store_get_server(principal, newkek, agent, cred, out, sizeof(out)) == -1);
   assert(vault_store_set_dual(principal, newkek, newkek, agent, cred, "x") == -1);
   assert(vault_store_add_server_wraps(principal, newkek, newkek) == -1);
   printf("  PASS: server dual-wrap ops unsupported (fail loud)\n");

   /* Cleanup so a re-run starts clean. */
   (void)vault_store_delete(principal, agent, cred);
}

int main(void)
{
   test_crypto_contract();

   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("vault_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }

   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-vault-pg-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "vault_pg: db2_init failed for %s\n", url);
      return 1;
   }
   vault_store_set_backend(&vault_pg_backend);

   run();

   db2_shutdown();
   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", home);
   (void)system(rm);
   printf("vault_pg: all tests passed\n");
   return 0;
}

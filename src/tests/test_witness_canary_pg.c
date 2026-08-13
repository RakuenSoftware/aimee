/* P7-witness-e3 §3: canary scan — no key material in witness evidence.
 *
 * The witness records and checkpoints are, by construction, digests and
 * identifiers: source_hash / witness_pred_hash / record_hash are SHA-256 outputs,
 * the checkpoint root is a Merkle root, the signature is an Ed25519 signature (made
 * WITH the private key but revealing nothing of it), signer_key_id is a hash of the
 * public key, and provider_cred is a stable routing identifier like
 * "anthropic:default". None of it is secret. This test proves that empirically
 * rather than by assertion:
 *
 *   - it extracts the REAL secrets a leak would expose — the server KEK, and the
 *     HKDF-derived witness signing SEED — and requires that NEITHER appears anywhere
 *     in the emitted evidence stream OR in a raw dump of the witness tables;
 *   - it plants a recognizable provider_cred sentinel and requires it to survive
 *     round-trip VERBATIM in the emitted record, proving the field is a passed-
 *     through stable identifier and was not transformed into a credential handle or
 *     a wrapped-key reference.
 *
 * Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) if unset. DESTRUCTIVE: run
 * against an isolated database only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db2_witness_checkpoint.h"
#include "db2/db2_witness_emit.h"
#include "db2/db_postgres.h"
#include "modules/vault/vault_witness_signer.h"

#define MUST(cond, ...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         fprintf(stderr, "FAILED (%s:%d): ", __FILE__, __LINE__);                                  \
         fprintf(stderr, __VA_ARGS__);                                                             \
         fprintf(stderr, "\n");                                                                    \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

/* vault_server_kek / the signer seed derivation live behind the signer; the KEK
 * accessor is declared in vault_server_key.h. */
#include "modules/vault/vault_server_key.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static uint8_t g_stream[1 << 20];
static size_t g_len;

static int capture_sink(void *ctx, vault_witness_export_kind_t kind, const uint8_t *frame,
                        size_t len)
{
   (void)ctx;
   (void)kind;
   if (g_len + len > sizeof g_stream)
      return -1;
   memcpy(g_stream + g_len, frame, len);
   g_len += len;
   return 0;
}

/* memmem is GNU; do a plain scan so the test needs no feature macros. */
static int contains(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn)
{
   if (nn == 0 || hn < nn)
      return 0;
   for (size_t i = 0; i + nn <= hn; i++)
      if (memcmp(hay + i, needle, nn) == 0)
         return 1;
   return 0;
}

#define SENTINEL_CRED "canary-cred-sentinel:anthropic:default"

static int append_with_cred(void *conn, const char *sid, const char *cred)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT * FROM org_vault_witness_append(0::smallint,?1,'!kb','!audit','',?2,?3,'g',"
       "'2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))",
       err, sizeof err);
   if (!st || aimee_pg_bind_text(st, "?1", sid) != 0 ||
       aimee_pg_bind_text(st, "?2", "canary-principal") != 0 ||
       aimee_pg_bind_text(st, "?3", cred) != 0)
   {
      if (st)
         aimee_pg_finalize(st);
      return -1;
   }
   int ok = (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

/* Dump every bytea/text column of the witness tables into one buffer, so the secret
 * scan covers what is actually stored, not only what is emitted. */
static size_t dump_witness_tables(void *conn, uint8_t *out, size_t cap)
{
   const char *q =
       "SELECT string_agg(x,'') FROM ("
       "  SELECT encode(source_hash,'hex')||encode(witness_pred_hash,'hex')||"
       "         encode(record_hash,'hex')||coalesce(provider_cred,'')||coalesce(source_id,'') AS x"
       "    FROM kb_vault_witness_log"
       "  UNION ALL"
       "  SELECT encode(root,'hex')||encode(signature,'hex')||encode(leaf_snapshot,'hex')||"
       "         encode(signer_key_id,'hex')||encode(leaf_snapshot_digest,'hex')"
       "    FROM kb_vault_witness_checkpoint"
       "  UNION ALL"
       "  SELECT encode(head_hash,'hex') FROM kb_vault_witness_shard) t";
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, q, err, sizeof err);
   if (!st)
      return 0;
   size_t n = 0;
   if (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      if (s)
      {
         n = strlen(s);
         if (n > cap)
            n = cap;
         memcpy(out, s, n);
      }
   }
   aimee_pg_finalize(st);
   return n;
}

/* Run a scalar count query with one text parameter bound at ?1. Returns the count,
 * or -1 on error. */
static int64_t count_query(void *conn, const char *sql, const char *arg)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof err);
   if (!st || (arg && aimee_pg_bind_text(st, "?1", arg) != 0))
   {
      if (st)
         aimee_pg_finalize(st);
      return -1;
   }
   int64_t v = -1;
   if (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
      v = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return v;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_canary_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-witness-canary-home-XXXXXX", platform_tmpdir());
   MUST(mkdtemp(home) != NULL, "mkdtemp failed");
   setenv("AIMEE_HOME", home, 1);
   MUST(db2_init(url) == 0, "db2_init failed for %s", url);
   void *conn = db2_conn();
   MUST(conn != NULL, "no connection");

   /* The secrets a leak would expose. */
   uint8_t kek[VAULT_KEK_LEN];
   MUST(vault_server_kek(kek) == 0, "could not read the server KEK");

   /* The witness signing SEED = HKDF-SHA256(ikm=KEK, salt="", info=SEED_INFO). It is
    * transient in production (derived, used, cleansed) with no accessor, so the
    * canary re-derives it with the exact production parameters and scans for it
    * INDEPENDENTLY of the KEK: the two are distinct byte strings, and a seed leak
    * (into a debug line, a core-dump fragment, a witness sub-field) would not show as
    * a KEK leak. */
   uint8_t seed[32];
   {
      EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
      size_t outlen = sizeof seed;
      int ok = c && EVP_PKEY_derive_init(c) == 1 &&
               EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1 &&
               EVP_PKEY_CTX_set1_hkdf_key(c, kek, VAULT_KEK_LEN) == 1 &&
               EVP_PKEY_CTX_add1_hkdf_info(c, (const unsigned char *)VAULT_WITNESS_SIGNER_SEED_INFO,
                                           (int)strlen(VAULT_WITNESS_SIGNER_SEED_INFO)) == 1 &&
               EVP_PKEY_derive(c, seed, &outlen) == 1 && outlen == sizeof seed;
      EVP_PKEY_CTX_free(c);
      MUST(ok, "could not re-derive the witness signing seed");
      MUST(memcmp(seed, kek, VAULT_KEK_LEN) != 0, "seed equals KEK — HKDF params wrong");
   }

   for (int i = 1; i <= 5; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "canary-%d", i);
      MUST(append_with_cred(conn, sid, SENTINEL_CRED) == 0, "append %s failed", sid);
   }
   int64_t cp = -1;
   MUST(db2_witness_checkpoint_produce(&cp) == DB2_WITNESS_CP_OK, "checkpoint produce failed");
   db2_witness_emit_stats_t s;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &s) == DB2_WITNESS_EMIT_OK, "emit failed");
   MUST(g_len > 0, "no evidence emitted");

   /* Guards against a vacuous pass. The KEK must be a real high-entropy value (an
    * all-zero KEK could "not appear" trivially), and the scanner itself must be able
    * to find the KEK in a buffer that genuinely contains it — otherwise a broken
    * contains() would make every absence check pass for free. */
   {
      uint8_t zero[VAULT_KEK_LEN] = {0};
      MUST(memcmp(kek, zero, VAULT_KEK_LEN) != 0,
           "server KEK is all-zero (canary would be vacuous)");
      uint8_t probe[VAULT_KEK_LEN + 8];
      memcpy(probe, "PREFIX--", 8);
      memcpy(probe + 8, kek, VAULT_KEK_LEN);
      MUST(contains(probe, sizeof probe, kek, VAULT_KEK_LEN),
           "scanner positive control failed: contains() cannot find the KEK it is looking for");
   }

   /* 1. Neither the server KEK nor the derived signing seed may appear in the
    *    emitted stream. */
   MUST(!contains(g_stream, g_len, kek, VAULT_KEK_LEN),
        "the server KEK appears in the emitted witness stream");
   MUST(!contains(g_stream, g_len, seed, sizeof seed),
        "the witness signing seed appears in the emitted witness stream");

   /* 2. Nor in a raw dump of the witness tables. The dump is hex-encoded, so compare
    *    against the hex of the KEK as well as the raw bytes. */
   static uint8_t dump[1 << 20];
   size_t dn = dump_witness_tables(conn, dump, sizeof dump);
   MUST(dn > 0, "witness table dump was empty");
   /* A truncated dump would let a secret hide past the cutoff and pass vacuously.
    * The canary works at small scale by design; if the dump ever fills the buffer,
    * fail loudly rather than scanning only a prefix. */
   MUST(dn < sizeof dump,
        "witness table dump was truncated at %zu bytes; the secret scan would "
        "only cover a prefix",
        sizeof dump);
   char kek_hex[VAULT_KEK_LEN * 2 + 1], seed_hex[32 * 2 + 1];
   for (int i = 0; i < VAULT_KEK_LEN; i++)
      snprintf(kek_hex + i * 2, 3, "%02x", kek[i]);
   for (int i = 0; i < 32; i++)
      snprintf(seed_hex + i * 2, 3, "%02x", seed[i]);
   MUST(!contains(dump, dn, (const uint8_t *)kek_hex, VAULT_KEK_LEN * 2),
        "the server KEK appears (hex) in a witness table");
   MUST(!contains(dump, dn, kek, VAULT_KEK_LEN), "the server KEK appears (raw) in a witness table");
   MUST(!contains(dump, dn, (const uint8_t *)seed_hex, 32 * 2),
        "the witness signing seed appears (hex) in a witness table");
   MUST(!contains(dump, dn, seed, 32), "the witness signing seed appears (raw) in a witness table");

   /* 3. provider_cred is a passed-through stable identifier. The sentinel must
    *    appear in the emitted record (proving it is emitted at all)... */
   MUST(contains(g_stream, g_len, (const uint8_t *)SENTINEL_CRED, strlen(SENTINEL_CRED)),
        "provider_cred sentinel did not survive round-trip in the emitted stream");
   /* ...and, in storage, it must be EXACTLY equal, not merely a substring. A
    * substring match would still pass if the producer wrapped the value
    * ("vault:<sentinel>") or stored a digest that happened to contain it. Exact SQL
    * equality proves no prefix/suffix/transform: all five canary rows equal the
    * sentinel, and none merely contains it. */
   int64_t exact =
       count_query(conn,
                   "SELECT count(*) FROM kb_vault_witness_log WHERE tenant='!kb' AND "
                   "provider='!audit' AND source_id LIKE 'canary-%' AND provider_cred=?1",
                   SENTINEL_CRED);
   int64_t wrapped =
       count_query(conn,
                   "SELECT count(*) FROM kb_vault_witness_log WHERE tenant='!kb' AND "
                   "provider='!audit' AND source_id LIKE 'canary-%' AND provider_cred<>?1 AND "
                   "position(?1 in provider_cred) > 0",
                   SENTINEL_CRED);
   MUST(exact == 5,
        "provider_cred is not stored verbatim: %lld of 5 canary rows equal the sentinel",
        (long long)exact);
   MUST(wrapped == 0,
        "provider_cred was wrapped/transformed (contains but does not equal the "
        "sentinel) in %lld rows",
        (long long)wrapped);

   OPENSSL_cleanse(kek, sizeof kek);
   OPENSSL_cleanse(seed, sizeof seed);
   db2_shutdown();
   printf("witness_canary_pg: PASSED (no KEK in evidence or tables; provider_cred is a stable "
          "identifier)\n");
   return 0;
}

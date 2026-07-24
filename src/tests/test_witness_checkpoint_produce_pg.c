/* P7-witness-e2 producer integration test (REAL PG ONLY).
 *
 * Drives db2_witness_checkpoint_produce() against a provisioned Postgres: appends
 * a couple of witness records, produces a signed checkpoint, and verifies the
 * persisted checkpoint's signature against the vault-derived public key. Reads
 * AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) if unset. AIMEE_HOME is pointed at a
 * throwaway dir so the vault server KEK (and thus the witness signing key) is
 * generated locally.
 */
#include <assert.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db2_witness_checkpoint.h"
#include "db2/db_postgres.h"
#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_signer.h"

static void append_record(void *conn, const char *sid)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT * FROM org_vault_witness_append(0::smallint,?1,'!kb','!audit','','p','','g',"
       "'2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))",
       err, sizeof err);
   assert(st && aimee_pg_bind_text(st, "?1", sid) == 0);
   aimee_pg_step_t sr = aimee_pg_step(st, err, sizeof err);
   assert(sr == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_checkpoint_produce_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   char home[] = "/tmp/aimee-witness-home-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "mkdtemp failed\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "db2_init failed for %s\n", url);
      return 1;
   }
   void *conn = db2_conn();
   assert(conn);

   /* Two witness records so the checkpoint has a non-empty shard. */
   append_record(conn, "1");
   append_record(conn, "2");

   /* Produce a checkpoint. */
   int64_t seq = -1;
   db2_witness_checkpoint_result_t r = db2_witness_checkpoint_produce(&seq);
   if (r != DB2_WITNESS_CP_OK)
   {
      fprintf(stderr, "produce returned %d\n", (int)r);
      return 1;
   }
   assert(seq >= 1);
   printf("witness_checkpoint_produce_pg: produced checkpoint seq=%lld\n", (long long)seq);

   /* Read it back and verify the signature against the vault-derived pubkey. */
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT root,has_predecessor,predecessor_digest,shard_count,leaf_snapshot_digest,"
       "signer_key_id,sig_alg,sig_version,signature,created_at "
       "FROM kb_vault_witness_checkpoint WHERE seq=?1",
       err, sizeof err);
   assert(st && aimee_pg_bind_int64(st, "?1", seq) == 0);
   assert(aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);

   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = (uint64_t)seq;
   /* Copy each bytea out before reading the next (single blob cache per stmt). */
   memcpy(cp.root, aimee_pg_column_blob(st, 0), 32);
   cp.has_predecessor = aimee_pg_column_text(st, 1)[0] == 't';
   memcpy(cp.predecessor_digest, aimee_pg_column_blob(st, 2), 32);
   cp.shard_count = (uint64_t)aimee_pg_column_int64(st, 3);
   memcpy(cp.leaf_snapshot_digest, aimee_pg_column_blob(st, 4), 32);
   memcpy(cp.signer_key_id, aimee_pg_column_blob(st, 5), VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   cp.sig_alg = (uint16_t)aimee_pg_column_int64(st, 6);
   cp.sig_version = (uint16_t)aimee_pg_column_int64(st, 7);
   memcpy(cp.signature, aimee_pg_column_blob(st, 8), 64);
   snprintf(cp.created_at, sizeof cp.created_at, "%s", aimee_pg_column_text(st, 9));
   aimee_pg_finalize(st);

   uint8_t pub[32], key_id[16];
   assert(vault_witness_signer_identity(pub, key_id) == 0);
   assert(memcmp(key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN) == 0);

   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   memcpy(anchor.ed25519_pub, pub, 32);
   vault_witness_cp_verdict_t v = vault_witness_checkpoint_verify(&cp, &anchor, 1);
   if (v != VAULT_WITNESS_CP_OK)
   {
      fprintf(stderr, "checkpoint signature verify failed: %d\n", (int)v);
      return 1;
   }

   db2_shutdown();
   printf("witness_checkpoint_produce_pg: PASSED (checkpoint signed by the vault key verifies)\n");
   return 0;
}

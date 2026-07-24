/* aimee-witness-boot-tpm-harness: prove the boot-REFUSAL path under a REAL custody
 * anchor (swtpm/TPM2), on a real Postgres.
 *
 * kb_witness_boot_check() only *refuses* when kb_vault_live_keys_allowed() — i.e.
 * under a real, unsealed anchor. Its two halves are proven independently elsewhere
 * (the tpm2 seal barrier flips live_keys TRUE in p7_tpm2_swtpm_test.sh; the
 * anchor-coverage query detects a foreign signer_key_id in test_witness_tamper_pg).
 * This harness proves the COMPOSITION that neither covers: that with live keys
 * actually allowed under a TPM anchor, the boot check
 *   - is a no-op while SEALED (no live keys yet),
 *   - returns 0 on evidence the current key can verify (does not spuriously refuse),
 *   - returns -1 on a retained checkpoint signed by a key this kb cannot derive.
 *
 * The blob is provisioned out of band (the driver runs test_vault_tpm2 provision);
 * this harness only unseals it. Must be built WITH_TPM2=1 for a real anchor — on a
 * stub build the anchor stays sealed and the harness reports that it could not enter
 * the live-keys regime, rather than passing vacuously.
 *
 * Usage: AIMEE_TEST_PG_URL=... AIMEE_VAULT_TPM2_TCTI=... AIMEE_VAULT_TPM2_BLOB_PATH=... \
 *          aimee-witness-boot-tpm-harness <unseal-secret>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db2_witness_checkpoint.h"
#include "db2/db_postgres.h"
#include "kb/kb_vault_policy.h"
#include "kb/kb_witness_cadence.h"
#include "log.h"
#include "modules/vault/vault_server_key.h"

#define CHECK(cond, ...)                                                                           \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         fprintf(stderr, "witness_boot_tpm: FAIL (%s:%d): ", __FILE__, __LINE__);                  \
         fprintf(stderr, __VA_ARGS__);                                                             \
         fprintf(stderr, "\n");                                                                    \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static int exec_sql(void *conn, const char *sql)
{
   char err[256];
   return aimee_pg_exec(conn, sql, err, sizeof err);
}

static int append_record(void *conn, const char *sid)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT * FROM org_vault_witness_append(0::smallint,?1,'!kb','!audit','','p','','g',"
       "'2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))",
       err, sizeof err);
   if (!st || aimee_pg_bind_text(st, "?1", sid) != 0)
   {
      if (st)
         aimee_pg_finalize(st);
      return -1;
   }
   int ok = (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
   if (argc != 2)
   {
      fprintf(stderr, "usage: %s <unseal-secret>\n", argv[0]);
      return 2;
   }
   const char *secret = argv[1];
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      fprintf(stderr, "witness_boot_tpm: AIMEE_TEST_PG_URL unset\n");
      return 2;
   }
   log_init(LOG_WARN);

   /* Bind the real TPM2 anchor through the kb policy seam. It boots SEALED. */
   char err[256] = "";
   CHECK(kb_vault_policy_select("tpm2", err, sizeof err) == 0, "bind tpm2 failed: %s", err);

   /* Sealed regime: no live keys, so the boot check is a no-op and must not touch
    * the DB or refuse. */
   CHECK(kb_vault_live_keys_allowed() == 0, "live keys allowed while still SEALED");
   CHECK(kb_witness_boot_check(err, sizeof err) == 0,
         "boot check refused while sealed (should be a no-op): %s", err);
   printf("witness_boot_tpm: sealed regime OK (no live keys, boot check is a no-op)\n");

   /* Unseal the real anchor. On a stub (non-WITH_TPM2) build this stays sealed. */
   if (vault_unseal(secret, strlen(secret)) != 0 || vault_is_sealed() != 0)
   {
      fprintf(stderr, "witness_boot_tpm: could not unseal a REAL anchor — this is a stub build or "
                      "swtpm is not wired; the boot-REFUSAL composition was NOT exercised\n");
      return 3; /* explicit: not a pass */
   }
   CHECK(kb_vault_live_keys_allowed() == 1, "live keys not allowed after unsealing a real anchor");
   printf("witness_boot_tpm: unsealed real anchor OK (live keys now allowed)\n");

   CHECK(db2_init(url) == 0, "db2_init failed for %s", url);
   void *conn = db2_conn();
   CHECK(conn != NULL, "no db connection");

   /* POSITIVE: real evidence signed by the current (TPM-derived) key must NOT be
    * refused — the gate must not fire spuriously under live keys. */
   for (int i = 1; i <= 3; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "boot-%d", i);
      CHECK(append_record(conn, sid) == 0, "seed append %s failed", sid);
   }
   int64_t seq = -1;
   CHECK(db2_witness_checkpoint_produce(&seq) == DB2_WITNESS_CP_OK, "checkpoint produce failed");
   CHECK(kb_witness_boot_check(err, sizeof err) == 0,
         "boot check REFUSED valid evidence under live keys (false positive): %s", err);
   printf("witness_boot_tpm: positive OK (live keys + verifiable evidence -> boot check passes)\n");

   /* NEGATIVE: a retained checkpoint signed by a key this kb cannot derive must make
    * the boot check refuse. The checkpoint table is WORM; an attacker (or a restored
    * foreign database) has already defeated that, so we disable the trigger to reach
    * the state and require the boot gate to catch it. */
   CHECK(exec_sql(conn, "ALTER TABLE kb_vault_witness_checkpoint DISABLE TRIGGER USER") == 0,
         "could not disable checkpoint WORM");
   CHECK(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint "
                        "SET signer_key_id = decode(repeat('be',16),'hex')") == 0,
         "foreign signer_key_id UPDATE failed");
   int refused = kb_witness_boot_check(err, sizeof err);
   CHECK(refused != 0,
         "boot check PASSED with a checkpoint signed by an underivable key (the refusal path is "
         "broken under live keys)");
   printf("witness_boot_tpm: negative OK (live keys + unverifiable checkpoint -> boot REFUSES: %s)\n",
          err);

   db2_shutdown();
   printf("witness_boot_tpm: PASSED (boot-refusal composition proven under a real TPM anchor)\n");
   return 0;
}

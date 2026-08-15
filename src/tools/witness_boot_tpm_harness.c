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
#include <time.h>

#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db2_witness_checkpoint.h"
#include "modules/db2/c/db_postgres.h"
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
   /* The release gate is closed while sealed — no live keys (term 1). */
   CHECK(kb_egress_release_allowed() == 0, "release gate OPEN while sealed (term 1 not fail-closed)");
   printf("witness_boot_tpm: sealed regime OK (no live keys, boot check no-op, gate closed)\n");

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

   /* ── The P2b release gate conjunction, term by term ─────────────────────────
    * The gate returns 1 ONLY on a live-key kb whose witnessing is healthy; every
    * term is fail-closed. We are under a real unsealed anchor, so terms 1-2 hold;
    * we drive terms 3-5 directly. */

   /* Term 5 fail-closed by default: verification has not run yet since boot, so the
    * gate must be CLOSED even though the evidence is valid. */
   CHECK(kb_egress_release_allowed() == 0,
         "gate OPEN before any verification pass (term 5 not fail-closed)");
   printf("witness_boot_tpm: gate closed pre-verification (term 5 fail-closed) OK\n");

   /* Drive the cadence until a continuous verification pass has run. The cadence
    * gates its whole body (checkpoint + verify + emit) behind one interval check, so
    * the first tick only arms the timer; a later tick past the (compressed) interval
    * runs the verify. Loop with real sleeps until last_clean flips off its -1
    * "never run" sentinel, bounded so a hang fails rather than spins. */
   setenv("AIMEE_WITNESS_CADENCE_TEST_S", "1", 1);
   {
      int ran = 0;
      for (int i = 0; i < 40; i++)
      {
         kb_witness_cadence_tick(time(NULL));
         if (kb_witness_verification_last_clean() != -1)
         {
            ran = 1;
            break;
         }
         struct timespec ts = {.tv_sec = 0, .tv_nsec = 300L * 1000 * 1000};
         nanosleep(&ts, NULL);
      }
      CHECK(ran, "the cadence never ran a verification pass");
   }
   CHECK(kb_witness_verification_last_clean() == 1,
         "verification did not report clean over a valid chain (last_clean=%d)",
         kb_witness_verification_last_clean());

   /* HEALTHY: every term holds -> gate OPEN. This is the whole point of the gate;
    * if this were 0 the gate would never open on a healthy production kb. */
   CHECK(kb_egress_release_allowed() == 1,
         "gate CLOSED on a fully healthy live-key kb (the gate would never open)");
   printf("witness_boot_tpm: gate OPEN on a healthy live-key kb OK\n");

   /* Term 4 — freshness. Age the latest checkpoint past the bound; the gate closes.
    * The checkpoint table is WORM, so disable the trigger to reach the state an
    * operator would see when the chain has stalled. */
   CHECK(exec_sql(conn, "ALTER TABLE kb_vault_witness_checkpoint DISABLE TRIGGER USER") == 0,
         "could not disable checkpoint WORM");
   CHECK(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint "
                        "SET created_at = (CURRENT_TIMESTAMP - interval '4000 seconds')::text") == 0,
         "aging created_at failed");
   CHECK(kb_egress_release_allowed() == 0, "gate OPEN with a stale checkpoint chain (term 4)");
   /* Restore freshness and confirm the gate re-opens — proving term 4 was the cause,
    * not a one-way latch. */
   CHECK(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint SET created_at = pg_now_text()") == 0,
         "restoring created_at failed");
   CHECK(kb_egress_release_allowed() == 1, "gate did not re-open after freshness restored (term 4)");
   printf("witness_boot_tpm: gate closes on a stale chain and re-opens when fresh (term 4) OK\n");

   /* Term 3 — anchor coverage. A retained checkpoint signed by a key this kb cannot
    * derive closes the gate AND makes the boot check refuse. */
   CHECK(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint "
                        "SET signer_key_id = decode(repeat('be',16),'hex')") == 0,
         "foreign signer_key_id UPDATE failed");
   CHECK(kb_egress_release_allowed() == 0,
         "gate OPEN with a checkpoint signed by an underivable key (term 3)");
   err[0] = '\0';
   int refused = kb_witness_boot_check(err, sizeof err);
   CHECK(refused != 0,
         "boot check PASSED with a checkpoint signed by an underivable key (the refusal path is "
         "broken under live keys)");
   /* Refuse for the RIGHT reason. boot_check also returns -1 when the coverage check
    * could not run ("could not be checked"); that would pass this assertion for the
    * wrong reason. Require the foreign-key message specifically. */
   CHECK(strstr(err, "cannot derive") != NULL,
         "boot check refused, but not via the foreign-key path (message: %s)", err);
   printf("witness_boot_tpm: gate closes + boot REFUSES on an underivable-key checkpoint (term 3): "
          "%s\n",
          err);

   db2_shutdown();
   printf("witness_boot_tpm: PASSED (boot-refusal + release-gate conjunction proven under a real "
          "TPM anchor)\n");
   return 0;
}

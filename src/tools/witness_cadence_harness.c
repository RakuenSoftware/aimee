/* aimee-witness-cadence-harness: drive the REAL production witness cadence in a
 * real, separately-killable process.
 *
 * This exists for the E3 §1 kill matrix's process-level boundaries. The full
 * aimee-kb daemon pulls in an embedder, HTTP, mTLS, and the management runtime —
 * none of which relate to witnessing and any of which can block a boot for
 * unrelated reasons. This harness drives exactly the two production entry points
 * the kb main loop calls — kb_witness_boot_check() at startup and
 * kb_witness_cadence_tick() every iteration — and nothing else, so a SIGKILL and
 * restart test the witness cadence itself rather than the daemon's unrelated
 * surface.
 *
 * It is custody-independent on purpose: the cadence signs with the KEK-derived
 * witness key, which exists under any custody, so no TPM/HSM/KMS is needed to prove
 * the cadence fires and survives a hard kill. (The boot-refusal path is a no-op
 * under file custody; exercising THAT needs a real custody anchor and is a separate
 * test.)
 *
 * Usage:
 *   AIMEE_TEST_PG_URL=... AIMEE_HOME=... AIMEE_WITNESS_CADENCE_TEST_S=1 \
 *     aimee-witness-cadence-harness
 *
 * On startup it prints one line to STDOUT:
 *   ANCHOR <32-hex key_id>:<64-hex ed25519 pubkey>
 * so a driver can build the trust-anchor file for the offline verifier. All
 * evidence rides stderr via the normal LOG_INFO("kb.witness.evidence", ...) path,
 * exactly as in production. The process runs until SIGTERM (clean stop) or SIGKILL
 * (the kill-matrix event); it prints "HARNESS STOPPED" only on the clean path.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "modules/db2/c/db2.h"
#include "modules/db2/c/db_postgres.h"
#include "kb/kb_witness_cadence.h"
#include "log.h"
#include "modules/vault/vault_witness_signer.h"

static volatile sig_atomic_t g_running = 1;

static void on_term(int sig)
{
   (void)sig;
   g_running = 0;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      fprintf(stderr, "harness: AIMEE_TEST_PG_URL unset\n");
      return 2;
   }
   log_init(LOG_INFO);
   signal(SIGTERM, on_term);
   signal(SIGINT, on_term);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "harness: db2_init failed\n");
      return 1;
   }

   /* Optionally downgrade to the low-privilege runtime role for everything after
    * schema apply, so the boot check and cadence execute exactly as they do on the
    * hardened tier (the kb connects as aimee_kb_runtime there). This is the live
    * check that the runtime-role grants are sufficient — schema was applied as the
    * owner (db2_init), but the cadence/boot/gate must run as the restricted role. */
   const char *role = getenv("AIMEE_WITNESS_HARNESS_ROLE");
   if (role && role[0])
   {
      char setrole[128], serr[256];
      snprintf(setrole, sizeof setrole, "SET ROLE %s", role);
      if (aimee_pg_exec(db2_conn(), setrole, serr, sizeof serr) != 0)
      {
         fprintf(stderr, "harness: SET ROLE %s failed: %s\n", role, serr);
         db2_shutdown();
         return 1;
      }
      fprintf(stderr, "harness: acting as role %s for boot check + cadence\n", role);
   }

   /* The real boot gate. Under file custody this is a no-op; under a real anchor it
    * would refuse to start on unverifiable evidence. Either way it is the exact
    * function the kb main path calls at boot. */
   char boot_err[256] = "";
   if (kb_witness_boot_check(boot_err, sizeof boot_err) != 0)
   {
      fprintf(stderr, "harness: boot check refused: %s\n", boot_err);
      db2_shutdown();
      return 3;
   }

   /* Publish the anchor so the driver can verify the emitted stream offline. */
   uint8_t pub[32], key_id[16];
   if (vault_witness_signer_identity(pub, key_id) == 0)
   {
      char line[16 * 2 + 1 + 32 * 2 + 1];
      size_t o = 0;
      for (int i = 0; i < 16; i++)
         o += (size_t)snprintf(line + o, sizeof line - o, "%02x", key_id[i]);
      line[o++] = ':';
      for (int i = 0; i < 32; i++)
         o += (size_t)snprintf(line + o, sizeof line - o, "%02x", pub[i]);
      printf("ANCHOR %s\n", line);
   }
   else
      printf("ANCHOR none\n");
   fflush(stdout);

   /* The main-loop shape, verbatim from kb_main: tick every 200ms with wall-clock
    * time. The cadence's own interval gate (AIMEE_WITNESS_CADENCE_TEST_S in tests)
    * decides when a checkpoint/emit/verify actually happens. */
   while (g_running)
   {
      kb_witness_cadence_tick(time(NULL));
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 200L * 1000 * 1000};
      nanosleep(&ts, NULL);
   }

   db2_shutdown();
   printf("HARNESS STOPPED\n");
   fflush(stdout);
   return 0;
}

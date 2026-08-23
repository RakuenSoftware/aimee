/* test_server_tls_init_cause.c — TLS startup must name WHICH failure.
 *
 * server_tls_init_default() has three unrelated failure paths: the mTLS ramp
 * self-test (DB1 stage 19, db1-pki), aimee's client CA, and the server identity
 * itself. All three returned -1, and the caller reported every one as
 *
 *     tls_port=8743 set but TLS cert/key not loadable; TLS DISABLED
 *
 * so a server whose certificate was perfectly good, but whose ramp could not
 * reach DB1, sent the operator to inspect the certificate. That happened during
 * this branch's own validation and cost real time before the db1 module was
 * identified as the actual missing piece.
 *
 * The live reproduction is staged by scripts/validation/fact-authority/
 * test-tls-failure-cause.sh, which SKIPS where the ramp completes without a db1
 * module. The mapping is deterministic, so it is pinned here instead of being
 * left to an environment that may or may not be able to fail.
 */
#include "server_tls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   printf("server_tls_init_cause:\n");

   /* Every cause is distinct, so a caller cannot report one as another. */
   const char *ramp = server_tls_init_result_str(SERVER_TLS_INIT_ERR_MTLS_RAMP);
   const char *ca = server_tls_init_result_str(SERVER_TLS_INIT_ERR_CLIENT_CA);
   const char *identity = server_tls_init_result_str(SERVER_TLS_INIT_ERR_IDENTITY);
   assert(ramp && ca && identity);
   assert(strcmp(ramp, ca) != 0);
   assert(strcmp(ramp, identity) != 0);
   assert(strcmp(ca, identity) != 0);

   /* The regression this exists for: the ramp failure must NOT talk about the
    * certificate, because the certificate is not what failed. */
   assert(strstr(ramp, "ramp"));
   assert(!strstr(ramp, "certificate"));
   assert(!strstr(ramp, "cert/key"));
   /* It should point at what actually has to be fixed. */
   assert(strstr(ramp, "db1"));

   /* And the identity failure still says so plainly. */
   assert(strstr(identity, "certificate"));

   /* Success is not an error string, and an out-of-range code must not name a
    * specific cause -- reporting "unknown" as e.g. a certificate fault is how
    * this class of defect starts. */
   assert(strcmp(server_tls_init_result_str(SERVER_TLS_INIT_OK), "ok") == 0);
   assert(strcmp(server_tls_init_result_str(-999), "unknown") == 0);
   assert(strcmp(server_tls_init_result_str(42), "unknown") == 0);

   printf("  PASS: each TLS startup failure names its own cause\n");
   printf("test_server_tls_init_cause: ok\n");
   return 0;
}

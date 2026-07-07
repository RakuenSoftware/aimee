/* test_vault_capability.c: the vault:write:server grants store (P2/D2c). */
#include "vault_capability.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
   const char *path = "/tmp/aimee-vault-cap-test-grants";
   unlink(path);
   vault_capability_set_path_for_test(path);

   char buf[256];

   /* empty store */
   assert(vault_capability_has("uid:1000") == 0);
   assert(vault_capability_list(buf, sizeof(buf)) == 0);

   /* grant + idempotency */
   assert(vault_capability_grant("uid:1000") == 0);
   assert(vault_capability_has("uid:1000") == 1);
   assert(vault_capability_grant("uid:1000") == 0);
   assert(vault_capability_has("uid:1000") == 1);

   /* a second principal */
   assert(vault_capability_grant("webuser:alice") == 0);
   assert(vault_capability_has("webuser:alice") == 1);
   assert(vault_capability_list(buf, sizeof(buf)) == 2);

   /* whole-line match only — a prefix/extension must NOT be granted */
   assert(vault_capability_has("uid:100") == 0);
   assert(vault_capability_has("uid:10000") == 0);

   /* revoke + idempotency */
   assert(vault_capability_revoke("uid:1000") == 0);
   assert(vault_capability_has("uid:1000") == 0);
   assert(vault_capability_has("webuser:alice") == 1);
   assert(vault_capability_list(buf, sizeof(buf)) == 1);
   assert(vault_capability_revoke("uid:1000") == 0);

   /* the server-principal write gate: attested transport AND capability.
    * webuser:alice is granted; uid:1000 is not (revoked above). */
   assert(vault_capability_server_write_allowed(ATTEST_UDS_PEERCRED, "webuser:alice") == 1);
   assert(vault_capability_server_write_allowed(ATTEST_WEBCHAT_TRUSTED, "webuser:alice") == 1);
   assert(vault_capability_server_write_allowed(ATTEST_TCP_BEARER, "webuser:alice") == 0);
   assert(vault_capability_server_write_allowed(ATTEST_NONE, "webuser:alice") == 0);
   assert(vault_capability_server_write_allowed(ATTEST_UDS_PEERCRED, "uid:1000") == 0);
   assert(vault_capability_server_write_allowed(ATTEST_UDS_PEERCRED, "") == 0);

   /* the agent-key server-seal gate: purely transport-based, NO grant needed
    * (deliberately wider than the server-write gate above), so a default install
    * seals a GUI/CLI-added delegate key without provisioning a capability first.
    * uid:1000 is NOT granted, yet its UDS transport is still allowed here. */
   assert(vault_agent_key_server_seal_allowed(ATTEST_TLS_BEARER) == 1);
   assert(vault_agent_key_server_seal_allowed(ATTEST_MTLS_CLIENT) == 1);
   assert(vault_agent_key_server_seal_allowed(ATTEST_WEBCHAT_TRUSTED) == 1); /* the HTTPS GUI */
   assert(vault_agent_key_server_seal_allowed(ATTEST_UDS_PEERCRED) == 1);    /* local CLI */
   assert(vault_agent_key_server_seal_allowed(ATTEST_TCP_BEARER) == 0);      /* D2b: plaintext */
   assert(vault_agent_key_server_seal_allowed(ATTEST_NONE) == 0);

   /* validation: empty / NULL / multi-line principals are rejected */
   assert(vault_capability_grant("") == -1);
   assert(vault_capability_grant(NULL) == -1);
   assert(vault_capability_grant("uid:1\n2") == -1);
   assert(vault_capability_has("") == 0);

   vault_capability_set_path_for_test(NULL);
   unlink(path);
   printf("PASS: vault_capability grant/revoke/has/list + whole-line + validation + "
          "agent-key server-seal gate\n");
   return 0;
}

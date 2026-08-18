/* identity_mint_live_main.c — mint one real data-plane identity token.
 *
 * The last unexercised step of the per-user /v1 write feature. Everything up to
 * here is reachable from psql, but identity_issue is not: it runs
 * admit -> use -> sign-under-vault-custody -> finalize in C, so proving it works
 * needs a binary that links the authority service and the vault objects. This is
 * that binary, and nothing more — it is a driver, not a new capability.
 *
 * It deliberately mirrors kb_mgmt_token_authority_main.c's preparation, because
 * the point is to exercise the SAME path the daemon takes rather than a
 * convenient approximation: the same custody provider, the same unseal/refresh/
 * ready sequence before any signing, the same service struct with a reopen
 * callback, and the same kb_mgmt_token_authority_service_issue entry point the
 * daemon hands to its IPC loop. What it drops is only the daemon itself — no
 * socket, no fd hardening, no privilege split — because a one-shot driver has no
 * peer to authenticate.
 *
 * Usage: identity-mint-live <dsn> <correlation_id_hex64> <jti_hex64>
 *
 * Requires the same environment the provisioners do (AIMEE_VAULT_KMS_HELPER,
 * _KEY_ID, _HWM_PUBKEY, _HWM_DOMAIN) and a raised RLIMIT_MEMLOCK. It prints the
 * minted JWT's structure and claim set — never the signature — and exits non-zero
 * with the IPC result name on refusal.
 */
#include "kb_mgmt_token_authority_ipc.h"
#include "kb/kb_mgmt_token_authority_service.h"
#include "modules/db2/c/management_token_authority.h"
#include "vault_custody_kms.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   const char *dsn;
} reopen_config_t;

static int reopen_database(void *opaque, db2_management_token_authority_ctx_t *db)
{
   reopen_config_t *config = opaque;
   char error[256] = "";
   if (!config || !db)
      return -1;
   db2_management_token_authority_close(db);
   int rc = db2_management_token_authority_open(db, config->dsn, error, sizeof(error));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

static const char *ipc_result_name(kb_mgmt_token_authority_ipc_result_t r)
{
   switch (r)
   {
   case KB_MGMT_TOKEN_AUTHORITY_IPC_OK:
      return "OK";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED:
      return "ALREADY_USED (the jti was already spent — a mint is single-use)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS:
      return "COMMIT_AMBIGUOUS (a lost COMMIT; terminal by design, never retried)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID:
      return "INVALID (a malformed identifier)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED:
      return "DENIED (authorization: the grant, the scope, or a role)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_CONFLICT:
      return "CONFLICT (a concurrent admission)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED:
      return "EXPIRED (the intent's window has closed)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED:
      return "SEALED (the vault sealed, or its epoch moved)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY:
      return "INTEGRITY (a binding or digest re-check failed)";
   case KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE:
      return "UNAVAILABLE";
   }
   return "UNKNOWN";
}

/* Print the JWT's shape and its payload, never the signature: the whole point of
 * the exercise is that the signature exists and was produced under custody, and
 * echoing it into a log would be the one genuinely sensitive part of the output. */
static void describe_token(const char *jwt)
{
   const char *d1 = strchr(jwt, '.');
   const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
   if (!d1 || !d2)
   {
      printf("  MALFORMED: not three dot-separated segments\n");
      return;
   }
   printf("  segments: header=%zu payload=%zu signature=%zu bytes\n", (size_t)(d1 - jwt),
          (size_t)(d2 - d1 - 1), strlen(d2 + 1));
   printf("  header.payload (base64url, verifiable against the published JWKS):\n    %.*s\n",
          (int)(d2 - jwt), jwt);
}

int main(int argc, char **argv)
{
   aimee_db2_register_token_record_validators(kb_mgmt_token_authority_record_valid,
                                              kb_identity_token_authority_record_valid);
   if (argc != 4)
   {
      fprintf(stderr, "usage: identity-mint-live <dsn> <correlation_id> <jti>\n");
      return 64;
   }
   const char *dsn = argv[1], *correlation_id = argv[2], *jti = argv[3];
   if (strlen(correlation_id) != 64 || strlen(jti) != 64)
   {
      fprintf(stderr, "identity-mint-live: correlation_id and jti must be 64 hex chars\n");
      return 64;
   }

   db2_management_token_authority_ctx_t database;
   memset(&database, 0, sizeof(database));
   char error[256] = "";
   if (db2_management_token_authority_open(&database, dsn, error, sizeof(error)) != 0)
   {
      fprintf(stderr, "identity-mint-live: database open failed: %s\n", error);
      OPENSSL_cleanse(error, sizeof(error));
      return 67;
   }

   reopen_config_t reopen = {.dsn = dsn};
   kb_mgmt_token_authority_service_t service = {
       .db = &database, .reopen_db = reopen_database, .reopen_opaque = &reopen};

   /* The same custody sequence the daemon runs before any signing can happen.
    * vault_is_sealed() and hwm_ready() are checked because unseal succeeding is
    * not the same as the HWM attestation being usable. */
   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0) != 0 || vault_custody_kms_hwm_refresh() != 0 || vault_is_sealed() ||
       !vault_custody_kms_hwm_ready())
   {
      fprintf(stderr, "identity-mint-live: vault custody not ready (unseal/HWM)\n");
      db2_management_token_authority_close(&database);
      return 68;
   }
   printf("vault: unsealed, HWM attestation ready\n");

   kb_mgmt_token_authority_output_t out;
   memset(&out, 0, sizeof(out));
   kb_mgmt_token_authority_ipc_result_t rc =
       kb_mgmt_token_authority_service_issue(correlation_id, jti, &out, &service);

   int exit_code = 0;
   if (rc != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
   {
      fprintf(stderr, "identity-mint-live: issue refused: rc=%d %s\n", (int)rc,
              ipc_result_name(rc));
      exit_code = 69;
   }
   else
   {
      printf("MINTED a data-plane identity token under vault custody\n");
      describe_token(out.jwt);
   }

   OPENSSL_cleanse(&out, sizeof(out));
   db2_management_token_authority_close(&database);
   if (vault_seal() != 0)
   {
      fprintf(stderr, "identity-mint-live: vault re-seal failed\n");
      exit_code = exit_code ? exit_code : 71;
   }
   return exit_code;
}

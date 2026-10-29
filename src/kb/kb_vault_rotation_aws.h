/* kb_vault_rotation_aws.h: rotate an org-vault Bedrock credential's IAM access key.
 *
 * An org-vault Bedrock slot holds one IAM user's access key as
 * {"access_key_id":"...","secret_access_key":"..."}. This provider rotates it
 * with that key alone (IAM self-rotation: the user may manage its own keys):
 *
 *   resolve    STS GetCallerIdentity and IAM ListAccessKeys with the current key;
 *              the user must hold exactly that one key, so nothing an operator
 *              owns is ever touched. The ARN, key id and start time are kept in
 *              the rotation's old vendor reference.
 *   provision  delete any key created since the rotation started (a lost earlier
 *              CreateAccessKey response), then CreateAccessKey.
 *   probe      STS GetCallerIdentity with the staged key; it must name the same
 *              user. A fresh key IAM has not propagated yet is uncertain, so the
 *              probe repeats.
 *   revoke     DeleteAccessKey with the active key, then ListAccessKeys must no
 *              longer list the revoked one. The active key is never revoked.
 *   reconcile  for remediation: report a key this rotation created and never
 *              staged, so it is revoked.
 *
 * Every use of a vault credential goes through kb_vault_key_use: admitted,
 * written to the WORM chain, decrypted only inside its callback. */
#ifndef AIMEE_KB_VAULT_ROTATION_AWS_H
#define AIMEE_KB_VAULT_ROTATION_AWS_H 1

#include "kb_identity.h"
#include "kb_vault_rotation_ops.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* One AWS Query API exchange: POST form to https://authority/. Returns 0 with
 * the HTTP status and the response body (error bodies included), 1 when the
 * request may have reached AWS but no response was read, -1 when it cannot have. */
typedef int (*kb_vault_rotation_aws_send_fn)(void *ctx, const char *authority,
                                             const char *const *header_names,
                                             const char *const *header_values, size_t header_count,
                                             const char *body, size_t body_len, int *status,
                                             char *response, size_t response_cap,
                                             size_t *response_len);

typedef struct
{
   kb_vault_rotation_aws_send_fn send;
   void *send_ctx;
   time_t (*now)(void);
   const char *iam_authority; /* "iam.amazonaws.com" */
   const char *sts_authority; /* "sts.amazonaws.com" */
} kb_vault_rotation_aws_t;

/* The production transport (kb_http_tls_exchange) and clock. */
void kb_vault_rotation_aws_defaults(kb_vault_rotation_aws_t *aws);

/* The provider vtable; pass a kb_vault_rotation_aws_t as its context. */
const kb_vault_rotation_provider_t *kb_vault_rotation_aws_provider(void);

/* The principal that steps a rotation, for the vault uses its callbacks make.
 * Set by the driver around each step on the calling thread; NULL clears it. */
void kb_vault_rotation_aws_set_caller(const kb_principal_t *caller, int64_t team_id);

#endif

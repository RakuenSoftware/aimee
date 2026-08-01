#ifndef AIMEE_KB_IDENTITY_TOKEN_H
#define AIMEE_KB_IDENTITY_TOKEN_H

/* kb_identity_token.h — the short-lived, kb-signed **data-plane identity token**
 * for per-user `remote_writes` authorization (proposal:
 * per-user-remote-writes-authz.md §4).
 *
 * This is deliberately a SEPARATE token type from the P5 management JWT
 * (kb_mgmt_token.h): it is carried to aimee-server by the user (browser / thin
 * client) on /v1, it has NO peer-cert binding and NO request digest, and it
 * carries a three-level write `tier`. Keeping it a distinct type (distinct
 * `typ`, distinct payload shape) means a data-plane token can never be replayed
 * on the management path and vice versa — the audience check is defense in
 * depth, not the only separation.
 *
 * The builder is pure: it never reads a clock, never mints an identifier, and
 * never touches private key material. The caller supplies fully-resolved claims
 * (including `jti`, `issued_at`, `expires_at`, `kid`) and a signer callback. It
 * is signed by the same kb token-authority key as the management JWT, so the
 * server verifies both against the same JWKS (by `kid`).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* The write tier the token grants. Wire form: the lowercase string in the
    * `tier` claim ("off" | "data" | "full"). Values match the server's
    * SERVER_REMOTE_WRITES_* ordering so a verified token maps straight onto the
    * existing gate parameter. */
   typedef enum
   {
      KB_IDENTITY_TIER_OFF = 0,
      KB_IDENTITY_TIER_DATA = 1,
      KB_IDENTITY_TIER_FULL = 2
   } kb_identity_tier_t;

   /* The pinned claim set (proposal §4). Every string field is NUL-terminated;
    * the builder rejects an unterminated field. `team_id` must be > 0. */
   typedef struct
   {
      char issuer[256];   /* iss — always the kb identity ("kb")               */
      char audience[128]; /* aud — the target server_id                        */
      char subject[577];  /* sub — OIDC (iss,sub) composite or PAM username    */
      int64_t team_id;    /* team — the tenant team the grant is scoped to     */
      kb_identity_tier_t tier;
      char jti[129];      /* unique token id (server-local replay rejection)   */
      char kid[65];       /* signing-key id (server selects the JWKS key)      */
      int64_t issued_at;  /* iat (unix seconds)                                */
      int64_t expires_at; /* exp (unix seconds); must be > issued_at           */
   } kb_identity_token_claims_t;

   typedef int (*kb_identity_token_sign_fn)(void *ctx, const unsigned char *signing_input,
                                            size_t signing_input_len, unsigned char *signature,
                                            size_t signature_cap, size_t *signature_len);

   typedef enum
   {
      KB_IDENTITY_TOKEN_OK = 0,
      KB_IDENTITY_TOKEN_INVALID,          /* malformed/again-unterminated claims  */
      KB_IDENTITY_TOKEN_SIGN_UNAVAILABLE, /* signer failed / produced no signature */
      KB_IDENTITY_TOKEN_OUTPUT_TOO_SMALL  /* jwt_cap too small for the wire token  */
   } kb_identity_token_result_t;

   /* Wire ceiling for a built token (header.payload.signature, base64url). */
#define KB_IDENTITY_TOKEN_WIRE_MAX 4096u

   /* Build the compact-serialization JWS (RS256) for the given claims. On
    * success writes a NUL-terminated token to jwt_out and its length to jwt_len.
    * The signer is called exactly once, after full validation and a worst-case
    * output preflight. Returns a non-OK result and an empty jwt_out otherwise. */
   kb_identity_token_result_t kb_identity_token_build(const kb_identity_token_claims_t *claims,
                                                      kb_identity_token_sign_fn signer,
                                                      void *signer_ctx, char *jwt_out,
                                                      size_t jwt_cap, size_t *jwt_len);

   /* The lowercase wire string for a tier ("off"/"data"/"full"), or NULL if the
    * enum is out of range. */
   const char *kb_identity_tier_str(kb_identity_tier_t tier);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_IDENTITY_TOKEN_H */

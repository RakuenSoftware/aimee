/* kb_store/enrollments.h — queryable redeemed-cert records for the kb web console
 * (accounts surface). Backed by the kb_enrollments table; the sha256 cert
 * fingerprint is the key, and revoked_at is the revocation source of truth. */
#ifndef KB_STORE_ENROLLMENTS_H
#define KB_STORE_ENROLLMENTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   int64_t id;
   char scope[128];
   char fingerprint[80]; /* sha256 lowercase hex (64) + NUL */
   char serial[80];
   char state[16]; /* active | revoked */
   char issued_at[32];
   char last_seen_at[32];
   char expires_at[32];
   char revoked_at[32];
   char authority_id[33]; /* renewal-stable random 128-bit lowercase hex */
   int legacy;
} kb_store_enrollment_row_t;

/* Insert (or upsert on fingerprint) a redeemed-cert record. legacy!=0 marks a
 * cert backfilled at first use rather than at redeem time. Returns 0, else -1. */
int kb_store_enrollment_insert(const char *scope, const char *fingerprint, const char *cert_issuer,
                               const char *cert_serial_norm, const char *expires_at, int legacy,
                               int64_t *out_id);

/* Atomically add a renewed certificate to the old certificate's stable
 * authority lineage, clone all canonical-principal grants, and append WORM
 * evidence. Returns 0, or -1 with no partial state. */
int kb_store_enrollment_renew(const char *old_fingerprint, const char *old_issuer,
                              const char *old_serial_norm, const char *scope,
                              const char *new_fingerprint, const char *new_issuer,
                              const char *new_serial_norm, int64_t *out_id);

/* List up to `max` rows, most-recent-first. Returns the count written, or -1. */
int kb_store_enrollment_list(int limit, kb_store_enrollment_row_t *out, int max);

/* Revoke by id: state='revoked', revoked_at=now. Fills `out` (if non-NULL) with
 * the revoked row. Returns 0, -1 on error, 1 if the id does not exist. */
int kb_store_enrollment_revoke(int64_t id, kb_store_enrollment_row_t *out);

/* Primary-authoritative per-request status by immutable certificate key.
 * Returns 1 only for an enrolled active identity, 0 for revoked or unknown,
 * and -1 when the authority cannot be queried or returns an invalid state.
 * Keep-alive/pooled request paths must require a return value of exactly 1. */
int kb_store_enrollment_is_active_by_key(const char *cert_issuer, const char *cert_serial_norm);

/* Resolve the stable egress authority for one exact active certificate instance.
 * Returns 0 and writes 32 hex chars, 1 when not active/enrolled, or -1 on error. */
int kb_store_enrollment_authority_resolve(const char *fingerprint, const char *cert_issuer,
                                          const char *cert_serial_norm, char out_authority[33]);

/* Best-effort, debounced "cert was used" write: bumps last_seen, and if the cert
 * predates this table (issued before S2a) backfills a legacy row for `scope` so
 * it becomes listable/revocable. Debounced so hot auth paths do not storm the DB.
 * The conflict path never touches state/revoked_at (a revoked cert stays revoked). */
void kb_store_enrollment_touch_last_seen(const char *fingerprint, const char *scope);

/* --- console OIDC login config (single row id=1) --- */
typedef struct
{
   char issuer[256];
   char audience[256];
   char jwks_url[512];
   char admin_claim[64];
   char admin_values[512]; /* comma-separated accepted values */
   char updated_at[32];
} kb_store_console_oidc_t;

/* Get the console OIDC config. 0 = found+filled, 1 = not configured, -1 = error. */
int kb_store_console_oidc_get(kb_store_console_oidc_t *out);
/* Upsert the single-row console OIDC config. Returns 0, or -1 on error. */
int kb_store_console_oidc_put(const kb_store_console_oidc_t *in);

#endif /* KB_STORE_ENROLLMENTS_H */

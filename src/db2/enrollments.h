/* db2/enrollments.h — queryable redeemed-cert records for the kb web console
 * (accounts surface). Backed by the kb_enrollments table; the sha256 cert
 * fingerprint is the key, and revoked_at is the revocation source of truth. */
#ifndef DB2_ENROLLMENTS_H
#define DB2_ENROLLMENTS_H

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
   int legacy;
} db2_enrollment_row_t;

/* Insert (or upsert on fingerprint) a redeemed-cert record. legacy!=0 marks a
 * cert backfilled at first use rather than at redeem time. Returns 0, else -1. */
int db2_enrollment_insert(const char *scope, const char *fingerprint, const char *serial,
                          const char *expires_at, int legacy, int64_t *out_id);

/* List up to `max` rows, most-recent-first. Returns the count written, or -1. */
int db2_enrollment_list(int limit, db2_enrollment_row_t *out, int max);

/* Revoke by id: state='revoked', revoked_at=now. Fills `out` (if non-NULL) with
 * the revoked row. Returns 0, -1 on error, 1 if the id does not exist. */
int db2_enrollment_revoke(int64_t id, db2_enrollment_row_t *out);

/* Is a cert (by fingerprint) revoked? 1 = revoked, 0 = active/unknown. Uses a
 * short-TTL in-process cache over the DB (revoked_at is the source of truth). */
int db2_enrollment_is_revoked(const char *fingerprint);

/* Best-effort, debounced "cert was used" write: bumps last_seen, and if the cert
 * predates this table (issued before S2a) backfills a legacy row for `scope` so
 * it becomes listable/revocable. Debounced so hot auth paths do not storm the DB.
 * The conflict path never touches state/revoked_at (a revoked cert stays revoked). */
void db2_enrollment_touch_last_seen(const char *fingerprint, const char *scope);

/* Drop the is-revoked cache (called after a revoke so the change is seen now). */
void db2_enrollment_cache_flush(void);

/* --- console OIDC login config (single row id=1) --- */
typedef struct
{
   char issuer[256];
   char audience[256];
   char jwks_url[512];
   char admin_claim[64];
   char admin_values[512]; /* comma-separated accepted values */
   char updated_at[32];
} db2_console_oidc_t;

/* Get the console OIDC config. 0 = found+filled, 1 = not configured, -1 = error. */
int db2_console_oidc_get(db2_console_oidc_t *out);
/* Upsert the single-row console OIDC config. Returns 0, or -1 on error. */
int db2_console_oidc_put(const db2_console_oidc_t *in);

#endif /* DB2_ENROLLMENTS_H */

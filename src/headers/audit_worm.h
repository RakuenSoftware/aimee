/* audit_worm.h: per-service WORM (write-once-read-many) audit store — S0.
 *
 * S0 of the "per-service auditable, verifiable WORM metrics-and-logs store"
 * proposal (docs/proposals/pending/auditable-worm-audit-store.md). This slice is
 * the aimee-server SQLite substrate: an append-only `audit_event` table with a
 * SHA-256 hash-chain and DB-level WORM triggers, a synchronous fsync-durable
 * single-writer append, and a chain verifier. It is wired behind the default-off
 * `audit_worm_enabled` config gate and dual-writes alongside the legacy
 * audit.log; the WORM store is not yet authoritative (that, plus MAC checkpoints
 * and OS-sealed segments, land in later slices S1/S2).
 *
 * ---- record + chain (identical across the future Postgres store) -------------
 *
 *   audit_event(seq, ts, actor_role, actor_principal, action, subject, verdict,
 *               detail, key_id, prev_hash, row_hash)
 *
 *   seq        1..N, gap-free (allocated MAX(seq)+1 inside the write txn)
 *   ts         RFC3339-UTC, ADVISORY — excluded from row_hash, not an ordering key
 *   row_hash   SHA256( AUDIT_WORM_DOMAIN "\n" prev_hash "\n" canonical(record) )
 *   prev_hash  the previous row's row_hash; for seq=1 the genesis is 32 zero bytes
 *              rendered as 64 hex '0's (AUDIT_WORM_GENESIS_PREV)
 *
 * canonical(record) is a length-prefixed concatenation of the hashed fields in a
 * FIXED order (see audit_worm.c). Length-prefixing makes it injective regardless
 * of field content, so it needs no general JSON canonicalizer; `detail` is hashed
 * as an opaque byte string in S0 (RFC 8785 canonicalization of a structured
 * `detail` schema is deferred to the detail-schema slice). The scheme is versioned
 * by AUDIT_WORM_DOMAIN so a later slice can bump it without breaking old rows.
 */
#ifndef AIMEE_AUDIT_WORM_H
#define AIMEE_AUDIT_WORM_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Domain-separation + algorithm tag folded into every row_hash. Bump to version
 * the canonicalization/hash. */
#define AUDIT_WORM_DOMAIN "aimee.audit.worm.v1"
/* Genesis prev_hash: 32 zero bytes as 64 lowercase hex chars. */
#define AUDIT_WORM_GENESIS_PREV "0000000000000000000000000000000000000000000000000000000000000000"

   /* Open (creating if needed) the WORM store at $AIMEE_HOME/audit/worm-live.db,
    * apply the schema + WORM triggers, and cache the handle. Idempotent. Returns 0
    * on success, -1 on failure. */
   int audit_worm_init(void);

   /* Test/embedding entry point: open a store at an explicit path. */
   int audit_worm_init_at(const char *db_path);

   /* Append one governed action. All string args are required (pass "" not NULL for
    * an absent field); `detail` is opaque canonical bytes. The row is fsync-durable
    * before this returns 0. Returns -1 on any failure (the caller decides whether to
    * fail closed; in S0 the dual-write path treats a failure as recoverable audit
    * loss because the legacy audit.log remains authoritative). */
   int audit_worm_append(const char *actor_role, const char *actor_principal, const char *action,
                         const char *subject, const char *verdict, const char *detail);

   /* Recompute the whole chain: for every row verify row_hash, prev_hash linkage,
    * and gap-free seq. Returns 0 if intact; -1 on the first break, writing a
    * human-readable reason into err (if non-NULL). MAC checkpoints extend this in
    * S1. */
   int audit_worm_verify_chain(char *err, size_t errlen);

   /* Append a first-class checkpoint row (action="chain.checkpoint") committing the
    * current chain head under a MAC keyed by the dedicated chain key
    * ($AIMEE_HOME/.audit-chain-key, created on first use, distinct from .audit-key),
    * so truncation/rollback past a checkpoint is detectable. 0 on success, -1 on
    * failure. */
   int audit_worm_checkpoint(void);

/* audit_worm_verify() status codes. */
#define AUDIT_WORM_VERIFY_GREEN 0 /* chain + MACs intact and head is checkpoint-attested */
#define AUDIT_WORM_VERIFY_AMBER                                                                    \
   1                            /* intact, but rows after the newest checkpoint are unattested     \
                                 */
#define AUDIT_WORM_VERIFY_RED 2 /* a break (hash, seq gap, or forged checkpoint MAC) */

   /* Full verify: the chain + every checkpoint MAC, plus the amber
    * uncheckpointed-tail signal. Returns one of AUDIT_WORM_VERIFY_*; writes a reason
    * into err on RED and fills head_seq / last_ckpt_seq when non-NULL. */
   int audit_worm_verify(char *err, size_t errlen, long *head_seq, long *last_ckpt_seq);

   /* Verify a sealed snapshot file (read-only) with the same chain + MAC checks.
    * 0 if intact, -1 on the first break (reason in err). */
   int audit_worm_verify_file(const char *db_path, char *err, size_t errlen);

   /* Seal an immutable point-in-time snapshot: checkpoint, VACUUM INTO
    * $AIMEE_HOME/audit/audit-sealed-<hi_seq>.db, then set the OS immutable flag
    * (best-effort — degrades to crypto-only without CAP_LINUX_IMMUTABLE / on an
    * unsupported FS). Fills out_path and *out_immutable (1 if kernel-immutable).
    * Returns 0 on a sealed snapshot, -1 on failure. */
   int audit_worm_seal(char *out_path, size_t out_cap, int *out_immutable);

   /* A page of the newest audit rows (seq DESC) as a cJSON array (caller owns);
    * fills *total with the full row count. Backs the WORM-sourced Logs/dashboard
    * read that supersedes the flat audit.log reader. */
   struct cJSON;
   struct cJSON *audit_worm_read_page(long offset, long limit, long *total);

   /* Number of rows currently in the store (test/introspection). -1 on error. */
   long audit_worm_count(void);

   /* Close the cached handle (idempotent; safe to call without a prior init). */
   void audit_worm_close(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_WORM_H */

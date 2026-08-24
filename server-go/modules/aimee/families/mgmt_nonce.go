package families

import (
	"context"
	"encoding/hex"
	"errors"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The management challenge postgres: a short-lived nonce per outstanding
// challenge, plus the revocation high-water mark a successful consume advances.
const (
	EventMgmtNonce uint32 = 11794
	StageMgmtNonce uint32 = 18

	opMgmtNonceClear    uint32 = 1
	opMgmtNonceIssue    uint32 = 2
	opMgmtNonceConsume  uint32 = 3
	opMgmtStatusHWMRead uint32 = 4
	opMgmtStatusHWMSet  uint32 = 5
)

// Result codes from mgmt_nonce.h. As elsewhere in this module these ride in the reply
// field, not the wire status.
const (
	nonceOK        = 0
	nonceNotFound  = 1
	nonceMismatch  = 2
	nonceExpired   = 3
	nonceRollback  = 4
	nonceInvalid   = 5
	nonceSaturated = 6
	nonceStorage   = 7
)

// nonceCap and nonceTTL are DB1_MGMT_NONCE_CAP and _TTL. The TTL is in seconds
// and is added to the caller's clock, so the store never reads its own.
const (
	nonceCap = 128
	nonceTTL = 15
)

const (
	nonceClearSQL = `DELETE FROM server_mgmt_nonce`

	nonceSweepSQL = `DELETE FROM server_mgmt_nonce WHERE expires_at < $1`

	nonceCountSQL = `SELECT count(*) FROM server_mgmt_nonce`

	nonceInsertSQL = `INSERT INTO server_mgmt_nonce
	                      (nonce, peer_issuer, peer_serial_norm, peer_fingerprint,
	                       channel_binding, target_server_id, purpose, expires_at)
	                  VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`

	nonceLookupSQL = `SELECT peer_issuer, peer_serial_norm, peer_fingerprint, channel_binding,
	                         target_server_id, purpose, expires_at
	                    FROM server_mgmt_nonce
	                   WHERE nonce = $1`

	nonceDeleteSQL = `DELETE FROM server_mgmt_nonce WHERE nonce = $1`

	hwmReadSQL = `SELECT generation FROM server_mgmt_status_hwm WHERE singleton`

	hwmBumpSQL = `UPDATE server_mgmt_status_hwm
	                 SET generation = greatest(generation, $1)
	               WHERE singleton`

	hwmSetSQL = `UPDATE server_mgmt_status_hwm
	                SET generation = $1
	              WHERE singleton AND generation <= $1`
)

// decodeNonce converts the wire's hex to the store's 32 bytes.
//
// Exactly 64 LOWERCASE hex characters, refused otherwise rather than decoded
// leniently: a truncated nonce would look up a different challenge, and Go's
// hex decoder would happily accept the uppercase spelling the C never did.
func decodeNonce(field string) ([]byte, bool) {
	if !lowercaseHex(field, 64, 64) {
		return nil, false
	}
	raw, err := hex.DecodeString(field)
	if err != nil {
		return nil, false
	}
	return raw, true
}

// nonceResult is the single-cell reply every nonce operation with an outcome
// answers with.
func nonceResult(code int) (uint32, []string, error) {
	return store.StatusOK, []string{store.Itoa(code)}, nil
}

// nonceClear is op 1: drop every outstanding challenge.
func nonceClear(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	if _, err := q.Exec(ctx, nonceClearSQL); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// nonceIssue is op 2: record a challenge the server just handed out.
//
// Sweep, then count, then insert -- in that order, so a store full of expired
// challenges is not reported as saturated. Saturation COMMITS the sweep for the
// same reason the jti stores do: discarding it would mean a saturated store can
// never drain.
func nonceIssue(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	nonce, ok := decodeNonce(f[0])
	if !ok {
		return nonceResult(nonceInvalid)
	}
	now, okNow := store.Atoi64(f[7])
	if !okNow {
		return nonceResult(nonceInvalid)
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return nonceResult(nonceStorage)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	if _, err := tx.Exec(ctx, nonceSweepSQL, now); err != nil {
		return nonceResult(nonceStorage)
	}
	var live int64
	if err := tx.QueryRow(ctx, nonceCountSQL).Scan(&live); err != nil {
		return nonceResult(nonceStorage)
	}
	if live >= nonceCap {
		if err := tx.Commit(ctx); err != nil {
			return nonceResult(nonceStorage)
		}
		return nonceResult(nonceSaturated)
	}
	// The expiry is the caller's clock plus the TTL. The store never consults
	// its own clock, so a caller and the store cannot disagree about now.
	if _, err := tx.Exec(ctx, nonceInsertSQL, nonce, f[1], f[2], f[3], f[4], f[5], f[6],
		now+nonceTTL); err != nil {
		return nonceResult(nonceStorage)
	}
	if err := tx.Commit(ctx); err != nil {
		return nonceResult(nonceStorage)
	}
	return nonceResult(nonceOK)
}

// nonceConsume is op 3: spend a challenge and, if everything checks out,
// advance the revocation high-water mark.
//
// The challenge is deleted whatever the verdict, which is the point: a
// challenge that was looked at is spent, and leaving a mismatched one in place
// would make a wrong answer a free retry.
func nonceConsume(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	nonce, ok := decodeNonce(f[0])
	if !ok {
		return nonceResult(nonceInvalid)
	}
	now, okNow := store.Atoi64(f[7])
	generation, okGen := store.Atoi64(f[8])
	valid, okValid := store.Atoi(f[9])
	if !okNow || !okGen || !okValid {
		return nonceResult(nonceInvalid)
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return nonceResult(nonceStorage)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var issuer, serial, fingerprint, binding, target, purpose string
	var expiresAt int64
	err = tx.QueryRow(ctx, nonceLookupSQL, nonce).Scan(&issuer, &serial, &fingerprint,
		&binding, &target, &purpose, &expiresAt)
	switch {
	case store.IsNoRows(err):
		// Nothing to spend. Committing rather than rolling back keeps this the
		// same shape as every other path, and there is nothing to undo.
		if err := tx.Commit(ctx); err != nil {
			return nonceResult(nonceStorage)
		}
		return nonceResult(nonceNotFound)
	case err != nil:
		return nonceResult(nonceStorage)
	}

	bound := issuer == f[1] && serial == f[2] && fingerprint == f[3] &&
		binding == f[4] && target == f[5] && purpose == f[6]

	tag, err := tx.Exec(ctx, nonceDeleteSQL, nonce)
	if err != nil {
		return nonceResult(nonceStorage)
	}
	if tag.RowsAffected() != 1 {
		// The row was there a statement ago. If it is not exactly one now,
		// something else is writing this table and the outcome is not safe to
		// report as a verdict.
		return nonceResult(nonceStorage)
	}

	switch {
	case !bound:
		return commitWith(ctx, tx, nonceMismatch)
	case expiresAt < 0 || now > expiresAt:
		return commitWith(ctx, tx, nonceExpired)
	case valid == 0:
		return commitWith(ctx, tx, nonceInvalid)
	}

	var hwm int64
	if err := tx.QueryRow(ctx, hwmReadSQL).Scan(&hwm); err != nil {
		return nonceResult(nonceStorage)
	}
	if generation < hwm {
		// A replayed status document: it carries a revocation generation older
		// than one already seen, so accepting it would roll the peer back to a
		// state where a revoked credential was still live.
		return commitWith(ctx, tx, nonceRollback)
	}
	tag, err = tx.Exec(ctx, hwmBumpSQL, generation)
	if err != nil {
		return nonceResult(nonceStorage)
	}
	if tag.RowsAffected() != 1 {
		return nonceResult(nonceStorage)
	}
	return commitWith(ctx, tx, nonceOK)
}

// commitWith commits the spend and reports the verdict. The DELETE has to
// persist for every one of these outcomes, so the verdict never decides whether
// to commit -- only what to say.
func commitWith(ctx context.Context, tx store.Tx, code int) (uint32, []string, error) {
	if err := tx.Commit(ctx); err != nil {
		return nonceResult(nonceStorage)
	}
	return nonceResult(code)
}

// hwmRead is op 4: the current revocation high-water mark.
func hwmRead(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var generation int64
	switch err := q.QueryRow(ctx, hwmReadSQL).Scan(&generation); {
	case store.IsNoRows(err):
		// The seed row is part of the schema, so its absence is a broken store
		// rather than a cold one.
		return 0, nil, errors.New("aimee: the status high-water mark row is missing")
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(generation)}, nil
}

// hwmSet is op 5: move the mark forward.
//
// Monotonic: the UPDATE carries `generation <= $1`, so a lower value matches no
// row and is refused rather than silently winding the mark back.
func hwmSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	generation, ok := store.Atoi64(f[0])
	if !ok || generation < 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, hwmSetSQL, generation)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		// Either the row is missing or the caller tried to move the mark
		// backwards. Both are failures rather than quiet successes.
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

// MgmtNonce is the family, ready to be bound to kind 11794.
var MgmtNonce = store.Family{
	Name:  "mgmt_nonce",
	Event: EventMgmtNonce,
	Stage: StageMgmtNonce,
	Ops: map[uint32]store.Op{
		opMgmtNonceClear:    {Name: "mgmt_nonce_clear", Args: 0, Tx: true, Run: nonceClear},
		opMgmtNonceIssue:    {Name: "mgmt_nonce_issue", Args: 8, RunDB: nonceIssue},
		opMgmtNonceConsume:  {Name: "mgmt_nonce_consume", Args: 10, RunDB: nonceConsume},
		opMgmtStatusHWMRead: {Name: "mgmt_status_hwm_read", Args: 0, Run: hwmRead},
		opMgmtStatusHWMSet:  {Name: "mgmt_status_hwm_set", Args: 1, Tx: true, Run: hwmSet},
	},
}

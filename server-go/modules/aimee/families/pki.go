package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The PKI roster and the mTLS enforcement ramp.
const (
	EventPKI uint32 = 11795
	StagePKI uint32 = 19

	opPKICertUpsert       uint32 = 1
	opPKICertList         uint32 = 2
	opPKIRevokedSerials   uint32 = 3
	opPKICertRevoke       uint32 = 4
	opPKICertCheck        uint32 = 5
	opPKINotePresentation uint32 = 6
	opPKIRampInit         uint32 = 7
	opPKIRampReady        uint32 = 8
	opPKIRampAdvance      uint32 = 9
	opPKIRampGet          uint32 = 10
)

// Certificate verdicts, from pki_store.h. UNKNOWN and ERROR are distinct on
// purpose: a serial that is not on this server's roster is a different fact
// from a roster this server could not read, and the caller fails closed on
// both but reports them differently.
const (
	certValid   = 0
	certRevoked = 1
	certExpired = 2
	certUnknown = 3
	certError   = 4
)

// Ramp states. 1 observes, 2 enforces; there is no state 0.
const (
	rampObserve = 1
	rampEnforce = 2
)

const (
	pkiSerialMax  = 127
	pkiCNMax      = 255
	pkiListMax    = 512
	rosterHashLen = 64
)

const (
	pkiCertInsertSQL = `INSERT INTO pki_certs (serial, cn, issued_at, expires_at, revoked, last_presented_at)
	                    VALUES ($1, $2, $3, $4, false, 0)`

	pkiCertRevokeSQL = `UPDATE pki_certs SET revoked = true WHERE serial = $1`

	pkiCertCheckSQL = `SELECT revoked, expires_at FROM pki_certs WHERE serial = $1`

	// greatest() rather than a CASE: the C spelled "keep the later of the two"
	// as a conditional because SQLite has no greatest(), and this is the same
	// rule said directly. The WHERE clause is what makes presenting a revoked
	// or expired certificate a no-op rather than a refresh.
	pkiNotePresentationSQL = `UPDATE pki_certs
	                             SET last_presented_at = greatest(last_presented_at, $1)
	                           WHERE serial = $2 AND NOT revoked
	                             AND (expires_at = 0 OR expires_at > $1)`

	pkiCertListSQL = `SELECT serial, cn, issued_at, expires_at, revoked
	                    FROM pki_certs
	                   ORDER BY issued_at DESC, serial
	                   LIMIT $1`

	pkiRevokedSerialsSQL = `SELECT serial FROM pki_certs WHERE revoked ORDER BY serial LIMIT $1`

	// The roster the ramp hashes: live certificates only, in a fixed order.
	pkiRosterSQL = `SELECT serial, cn, issued_at, expires_at, last_presented_at
	                  FROM pki_certs
	                 WHERE NOT revoked AND (expires_at = 0 OR expires_at > $1)
	                 ORDER BY serial, cn`

	rampReadSQL = `SELECT ramp_state, roster_hash, last_advance_ts
	                 FROM pki_mtls_ramp WHERE singleton`

	rampInsertSQL = `INSERT INTO pki_mtls_ramp (singleton, ramp_state, roster_hash, last_advance_ts)
	                      VALUES (true, $1, $2, $3)
	                 ON CONFLICT (singleton) DO NOTHING`

	rampSetHashSQL = `UPDATE pki_mtls_ramp SET roster_hash = $1 WHERE singleton`

	// The hash in the WHERE clause is the point: the advance only lands if the
	// roster is still the one that was judged ready, so a certificate added
	// between the check and the write cannot be enforced against.
	rampAdvanceSQL = `UPDATE pki_mtls_ramp
	                     SET ramp_state = 2, last_advance_ts = $1
	                   WHERE singleton AND ramp_state = 1 AND roster_hash = $2`

	rampForceEnforceSQL = `UPDATE pki_mtls_ramp
	                          SET ramp_state = 2, roster_hash = $1, last_advance_ts = $2
	                        WHERE singleton AND ramp_state = 1`
)

// roster is what the ramp measures: the live certificates, their hash, and
// whether every one of them has been presented since it was issued.
type roster struct {
	hash  string
	count int
	ready bool
}

// snapshotRoster hashes the live roster.
//
// The digest must match the C's byte for byte, because it is stored and
// compared against what the C wrote: sha256 over each row as
// serial NUL cn NUL "issued:expires" NUL, in (serial, cn) order.
func snapshotRoster(ctx context.Context, q store.Queryer, now int64) (roster, error) {
	rows, err := q.Query(ctx, pkiRosterSQL, now)
	if err != nil {
		return roster{}, err
	}
	defer rows.Close()

	digest := sha256.New()
	out := roster{ready: true}
	for rows.Next() {
		var serial, cn string
		var issued, expires, presented int64
		if err := rows.Scan(&serial, &cn, &issued, &expires, &presented); err != nil {
			return roster{}, err
		}
		digest.Write([]byte(serial))
		digest.Write([]byte{0})
		digest.Write([]byte(cn))
		digest.Write([]byte{0})
		digest.Write([]byte(fmt.Sprintf("%d:%d", issued, expires)))
		digest.Write([]byte{0})
		// A certificate that has never been presented, or was last presented
		// before it was issued, means the fleet has not finished picking up
		// its new material -- so enforcement is not safe yet.
		if presented <= 0 || presented < issued {
			out.ready = false
		}
		out.count++
	}
	if err := rows.Err(); err != nil {
		return roster{}, err
	}
	out.hash = hex.EncodeToString(digest.Sum(nil))
	out.ready = out.ready && out.count > 0
	return out, nil
}

func readRamp(ctx context.Context, q store.Queryer) (state int, hash string, advancedAt int64, err error) {
	err = q.QueryRow(ctx, rampReadSQL).Scan(&state, &hash, &advancedAt)
	return
}

// pkiCertUpsert is op 1. It is an INSERT, not an upsert, despite the name the
// catalog gives it: the C inserted and failed on a duplicate serial, and a
// certificate serial that already exists is a different certificate claiming an
// identity, not a refresh of this one.
func pkiCertUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	serial, cn := f[0], f[1]
	issuedAt, okIssued := store.Atoi64(f[2])
	expiresAt, okExpires := store.Atoi64(f[3])
	if serial == "" || len(serial) > pkiSerialMax || len(cn) > pkiCNMax ||
		!okIssued || !okExpires {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, pkiCertInsertSQL, serial, cn, issuedAt, expiresAt); err != nil {
		return 0, nil, err
	}
	// The roster changed, so the stored hash no longer describes it.
	if err := refreshRosterHash(ctx, q, issuedAt); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// refreshRosterHash re-stores the hash after the roster changes.
//
// It does NOT advance or retreat the ramp: a roster change while observing
// simply means the next readiness check measures the new roster, and a roster
// change while enforcing does not un-enforce.
func refreshRosterHash(ctx context.Context, q store.Queryer, now int64) error {
	snapshot, err := snapshotRoster(ctx, q, now)
	if err != nil {
		return err
	}
	if _, err := q.Exec(ctx, rampSetHashSQL, snapshot.hash); err != nil {
		return err
	}
	return nil
}

// pkiCertRevoke is op 4.
func pkiCertRevoke(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, pkiCertRevokeSQL, f[0]); err != nil {
		return 0, nil, err
	}
	// Revoking removes a certificate from the live roster, so the hash moves.
	// now is 0 here deliberately: the roster filter only drops certificates
	// that have already expired, and a revoke must not also sweep those out of
	// the hash as a side effect of the clock it happens to run at.
	if err := refreshRosterHash(ctx, q, 0); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// pkiCertCheck is op 5: the verdict for one serial.
//
// Every path fails closed. An unreadable roster is ERROR rather than VALID, and
// an unknown serial is UNKNOWN rather than either -- the caller refuses on all
// three, but it reports them differently and an operator needs to know which.
func pkiCertCheck(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	verdict := func(v int) (uint32, []string, error) {
		return store.StatusOK, []string{store.Itoa(v)}, nil
	}
	now, okNow := store.Atoi64(f[1])
	if f[0] == "" || !okNow {
		return verdict(certUnknown)
	}
	var revoked bool
	var expiresAt int64
	switch err := q.QueryRow(ctx, pkiCertCheckSQL, f[0]).Scan(&revoked, &expiresAt); {
	case store.IsNoRows(err):
		return verdict(certUnknown)
	case err != nil:
		return verdict(certError)
	}
	switch {
	case revoked:
		return verdict(certRevoked)
	case expiresAt > 0 && expiresAt <= now:
		return verdict(certExpired)
	}
	return verdict(certValid)
}

// pkiNotePresentation is op 6: record that a certificate was seen.
//
// Exactly one row must change. A serial that is revoked, expired or absent
// matches nothing, and reporting success for it would tell the ramp a
// certificate had been picked up when it had not -- which is the input the
// enforcement decision is made from.
func pkiNotePresentation(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	now, okNow := store.Atoi64(f[1])
	if f[0] == "" || !okNow {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, pkiNotePresentationSQL, now, f[0])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

// pkiCertList is op 2.
func pkiCertList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > pkiListMax {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, pkiCertListSQL, max)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	cells := make([]string, 0, max*5)
	for rows.Next() {
		var serial, cn string
		var issued, expires int64
		var revoked bool
		if err := rows.Scan(&serial, &cn, &issued, &expires, &revoked); err != nil {
			return 0, nil, err
		}
		cells = append(cells, serial, cn, store.I64toa(issued),
			store.I64toa(expires), store.Btoa(revoked))
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// pkiRevokedSerials is op 3.
func pkiRevokedSerials(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > pkiListMax {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, pkiRevokedSerialsSQL, max)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	cells := make([]string, 0, max)
	for rows.Next() {
		var serial string
		if err := rows.Scan(&serial); err != nil {
			return 0, nil, err
		}
		cells = append(cells, serial)
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// pkiRampInit is op 7: establish the ramp row and reconcile it with configuration.
//
// Configuration can only ever move the ramp FORWARD. A deployment configured to
// enforce promotes an observing ramp; a deployment configured to observe does
// not demote one that is already enforcing, because that would silently reopen
// a door the operator had closed.
func pkiRampInit(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	configured, ok := store.Atoi(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if configured <= 0 {
		// Not configured for mTLS at all: the C returned the mode unchanged
		// without touching the store.
		return store.StatusOK, []string{store.Itoa(configured)}, nil
	}

	// The ramp's own clock. init is the one operation the caller gives no time
	// to, so the database supplies it.
	var now int64
	if err := q.QueryRow(ctx, `SELECT extract(epoch FROM now())::bigint`).Scan(&now); err != nil {
		return 0, nil, err
	}

	snapshot, err := snapshotRoster(ctx, q, now)
	if err != nil {
		return 0, nil, err
	}

	initial := rampObserve
	advancedAt := int64(0)
	if configured >= rampEnforce {
		initial, advancedAt = rampEnforce, now
	}
	if _, err := q.Exec(ctx, rampInsertSQL, initial, snapshot.hash, advancedAt); err != nil {
		return 0, nil, err
	}

	state, hash, _, err := readRamp(ctx, q)
	if err != nil {
		return 0, nil, err
	}
	if (state != rampObserve && state != rampEnforce) || len(hash) != rosterHashLen {
		return 0, nil, errors.New("aimee: the ramp row is malformed")
	}

	if configured >= rampEnforce && state < rampEnforce {
		if _, err := q.Exec(ctx, rampForceEnforceSQL, snapshot.hash, now); err != nil {
			return 0, nil, err
		}
		state = rampEnforce
	} else if state == rampObserve {
		if _, err := q.Exec(ctx, rampSetHashSQL, snapshot.hash); err != nil {
			return 0, nil, err
		}
	}
	return store.StatusOK, []string{store.Itoa(state)}, nil
}

// rampReadiness answers "may enforcement begin", and optionally begins it.
//
// Already enforcing is reported as NOT ready: the question is whether there is
// an advance to make, and there is not.
func rampReadiness(ctx context.Context, q store.Queryer, now int64, advance bool) (int, error) {
	snapshot, err := snapshotRoster(ctx, q, now)
	if err != nil {
		return 0, err
	}
	state, stored, _, err := readRamp(ctx, q)
	switch {
	case store.IsNoRows(err):
		return 0, errors.New("aimee: the ramp has not been initialised")
	case err != nil:
		return 0, err
	}
	if state != rampObserve && state != rampEnforce {
		return 0, errors.New("aimee: the ramp is in no valid state")
	}
	if state == rampEnforce {
		return 0, nil
	}
	// The stored hash must still describe the roster. If it does not, the
	// roster changed since it was last recorded and this readiness answer is
	// about a fleet that no longer exists.
	if !snapshot.ready || snapshot.count == 0 || stored != snapshot.hash {
		return 0, nil
	}
	if !advance {
		return 1, nil
	}
	tag, err := q.Exec(ctx, rampAdvanceSQL, now, snapshot.hash)
	if err != nil {
		return 0, err
	}
	if tag.RowsAffected() == 1 {
		return 1, nil
	}
	// Someone else advanced it, or the roster moved under the write. Neither is
	// an error: the answer is just that this call did not advance it.
	return 0, nil
}

// pkiRampReady is op 8.
func pkiRampReady(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	now, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	ready, err := rampReadiness(ctx, q, now, false)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Itoa(ready)}, nil
}

// pkiRampAdvance is op 9.
func pkiRampAdvance(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	now, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	advanced, err := rampReadiness(ctx, q, now, true)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Itoa(advanced)}, nil
}

// pkiRampGet is op 10.
func pkiRampGet(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	state, hash, advancedAt, err := readRamp(ctx, q)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if len(hash) != rosterHashLen {
		return 0, nil, errors.New("aimee: the stored roster hash is not a sha256")
	}
	return store.StatusOK, []string{
		store.Itoa(state), hash, store.I64toa(advancedAt),
	}, nil
}

// PKI is the family, ready to be bound to kind 11795.
var PKI = store.Family{
	Name:  "pki",
	Event: EventPKI,
	Stage: StagePKI,
	Ops: map[uint32]store.Op{
		opPKICertUpsert:       {Name: "pki_cert_upsert", Args: 4, Tx: true, Run: pkiCertUpsert},
		opPKICertList:         {Name: "pki_cert_list", Cells: 5, Args: 1, Run: pkiCertList},
		opPKIRevokedSerials:   {Name: "pki_revoked_serials", Cells: 1, Args: 1, Run: pkiRevokedSerials},
		opPKICertRevoke:       {Name: "pki_cert_revoke", Args: 1, Tx: true, Run: pkiCertRevoke},
		opPKICertCheck:        {Name: "pki_cert_check", Args: 2, Run: pkiCertCheck},
		opPKINotePresentation: {Name: "pki_note_presentation", Args: 2, Tx: true, Run: pkiNotePresentation},
		opPKIRampInit:         {Name: "pki_ramp_init", Args: 1, Tx: true, Run: pkiRampInit},
		opPKIRampReady:        {Name: "pki_ramp_ready", Args: 1, Tx: true, Run: pkiRampReady},
		opPKIRampAdvance:      {Name: "pki_ramp_advance", Args: 1, Tx: true, Run: pkiRampAdvance},
		opPKIRampGet:          {Name: "pki_ramp_get", Cells: 3, Args: 0, Run: pkiRampGet},
	},
}

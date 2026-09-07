package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a challenge-store-shaped fake -------------------------------------------

type challenge struct {
	issuer      string
	serial      string
	fingerprint string
	binding     string
	target      string
	purpose     string
	expiresAt   int64
}

type nonceRow struct {
	db  *nonceDB
	sql string
}

func (r nonceRow) Scan(dest ...any) error {
	switch {
	case strings.Contains(r.sql, "count(*)"):
		*(dest[0].(*int64)) = r.db.liveCount
		return r.db.countErr
	case strings.Contains(r.sql, "FROM server_mgmt_status_hwm"):
		if r.db.hwmMissing {
			return store.ErrNoRows
		}
		*(dest[0].(*int64)) = r.db.hwm
		return r.db.hwmErr
	default: // the challenge lookup
		if r.db.challenge == nil {
			return store.ErrNoRows
		}
		c := r.db.challenge
		*(dest[0].(*string)) = c.issuer
		*(dest[1].(*string)) = c.serial
		*(dest[2].(*string)) = c.fingerprint
		*(dest[3].(*string)) = c.binding
		*(dest[4].(*string)) = c.target
		*(dest[5].(*string)) = c.purpose
		*(dest[6].(*int64)) = c.expiresAt
		return r.db.lookupErr
	}
}

type nonceTx struct{ db *nonceDB }

func (t *nonceTx) Exec(_ context.Context, sql string, _ ...any) (store.Tag, error) {
	t.db.executed = append(t.db.executed, sql)
	switch {
	case strings.Contains(sql, "DELETE FROM server_mgmt_nonce WHERE nonce"):
		return store.RowsAffected(t.db.deleteRows), t.db.deleteErr
	case strings.Contains(sql, "UPDATE server_mgmt_status_hwm"):
		return store.RowsAffected(t.db.hwmRows), t.db.hwmUpdateErr
	default:
		return store.RowsAffected(1), nil
	}
}
func (t *nonceTx) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (t *nonceTx) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	t.db.executed = append(t.db.executed, sql)
	return nonceRow{db: t.db, sql: sql}
}
func (t *nonceTx) Commit(context.Context) error   { t.db.committed = true; return t.db.commitErr }
func (t *nonceTx) Rollback(context.Context) error { t.db.rolledBack = true; return nil }

type nonceDB struct {
	challenge  *challenge
	liveCount  int64
	hwm        int64
	hwmMissing bool
	deleteRows int
	hwmRows    int

	countErr     error
	lookupErr    error
	deleteErr    error
	hwmErr       error
	hwmUpdateErr error
	commitErr    error

	executed   []string
	committed  bool
	rolledBack bool
}

func newNonceDB() *nonceDB { return &nonceDB{deleteRows: 1, hwmRows: 1} }

func (d *nonceDB) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	return (&nonceTx{db: d}).Exec(ctx, sql, args...)
}
func (d *nonceDB) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (d *nonceDB) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	d.executed = append(d.executed, sql)
	return nonceRow{db: d, sql: sql}
}
func (d *nonceDB) Begin(context.Context) (store.Tx, error) { return &nonceTx{db: d}, nil }

const goodNonce = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

// consumeFields is a request that matches storedChallenge() exactly, so a test
// can change one thing and know that is what it measured.
func consumeFields() []string {
	return []string{
		goodNonce, "CN=issuer", "00ff", "fingerprint", "binding", "server-1", "attest",
		"1000", // now
		"5",    // revocation_generation
		"1",    // valid
	}
}

func storedChallenge() *challenge {
	return &challenge{
		issuer: "CN=issuer", serial: "00ff", fingerprint: "fingerprint",
		binding: "binding", target: "server-1", purpose: "attest", expiresAt: 2000,
	}
}

func nonceCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := MgmtNonce.Handler(db)(
		bus.ModuleInvocation{StageID: StageMgmtNonce}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

func nonceOutcome(t *testing.T, db store.DB, op uint32, fields []string) int {
	t.Helper()
	status, cells := nonceCall(t, db, op, fields)
	if status != store.StatusOK {
		t.Fatalf("in-band status = %d, want OK: the outcome rides in the cell", status)
	}
	if len(cells) != 1 {
		t.Fatalf("reply carried %d cells, want 1", len(cells))
	}
	value, ok := store.Atoi(cells[0])
	if !ok {
		t.Fatalf("outcome cell %q is not a number", cells[0])
	}
	return value
}

// --- consume: the verdict ladder ---------------------------------------------

// The order of these checks is the security property. A mismatched challenge
// must not be reported as expired, and neither may reach the high-water mark
// check -- each rung is what stops the next one from being asked.
func TestConsumeVerdictLadder(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*nonceDB, []string) []string
		want   int
	}{
		{
			name:   "everything matches",
			mutate: func(*nonceDB, []string) []string { return consumeFields() },
			want:   nonceOK,
		},
		{
			name: "no such challenge",
			mutate: func(db *nonceDB, f []string) []string {
				db.challenge = nil
				return f
			},
			want: nonceNotFound,
		},
		{
			name:   "peer issuer differs",
			mutate: func(_ *nonceDB, f []string) []string { f[1] = "CN=someone-else"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "peer serial differs",
			mutate: func(_ *nonceDB, f []string) []string { f[2] = "beef"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "peer fingerprint differs",
			mutate: func(_ *nonceDB, f []string) []string { f[3] = "other-fingerprint"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "target server differs",
			mutate: func(_ *nonceDB, f []string) []string { f[5] = "server-2"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "channel binding differs",
			mutate: func(_ *nonceDB, f []string) []string { f[4] = "other-binding"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "purpose differs",
			mutate: func(_ *nonceDB, f []string) []string { f[6] = "something-else"; return f },
			want:   nonceMismatch,
		},
		{
			name:   "expired",
			mutate: func(_ *nonceDB, f []string) []string { f[7] = "3000"; return f },
			want:   nonceExpired,
		},
		{
			name:   "caller says the request was not valid",
			mutate: func(_ *nonceDB, f []string) []string { f[9] = "0"; return f },
			want:   nonceInvalid,
		},
		{
			name: "revocation generation older than the mark",
			mutate: func(db *nonceDB, f []string) []string {
				db.hwm = 9 // the request carries 5
				return f
			},
			want: nonceRollback,
		},
		{
			name: "generation equal to the mark is accepted",
			mutate: func(db *nonceDB, f []string) []string {
				db.hwm = 5
				return f
			},
			want: nonceOK,
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			db := newNonceDB()
			db.challenge = storedChallenge()
			fields := test.mutate(db, consumeFields())
			if got := nonceOutcome(t, db, opMgmtNonceConsume, fields); got != test.want {
				t.Fatalf("outcome = %d, want %d", got, test.want)
			}
		})
	}
}

// A challenge that was looked at is spent, whatever the verdict. Leaving a
// mismatched one in place would make a wrong answer a free retry.
func TestConsumeSpendsTheChallengeOnEveryVerdict(t *testing.T) {
	for _, test := range []struct {
		name   string
		mutate func([]string) []string
	}{
		{"accepted", func(f []string) []string { return f }},
		{"mismatch", func(f []string) []string { f[1] = "CN=wrong"; return f }},
		{"expired", func(f []string) []string { f[7] = "3000"; return f }},
		{"not valid", func(f []string) []string { f[9] = "0"; return f }},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newNonceDB()
			db.challenge = storedChallenge()
			nonceOutcome(t, db, opMgmtNonceConsume, test.mutate(consumeFields()))
			var deleted bool
			for _, sql := range db.executed {
				if strings.Contains(sql, "DELETE FROM server_mgmt_nonce WHERE nonce") {
					deleted = true
				}
			}
			if !deleted {
				t.Fatalf("the challenge was not spent")
			}
			if !db.committed {
				t.Fatalf("the spend was not committed")
			}
		})
	}
}

// Only an accepted consume touches the high-water mark. A mismatched or expired
// challenge advancing it would let a rejected peer move the revocation state.
func TestOnlyAnAcceptedConsumeAdvancesTheMark(t *testing.T) {
	for _, test := range []struct {
		name   string
		mutate func([]string) []string
		bump   bool
	}{
		{"accepted", func(f []string) []string { return f }, true},
		{"mismatch", func(f []string) []string { f[1] = "CN=wrong"; return f }, false},
		{"expired", func(f []string) []string { f[7] = "3000"; return f }, false},
		{"not valid", func(f []string) []string { f[9] = "0"; return f }, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newNonceDB()
			db.challenge = storedChallenge()
			nonceOutcome(t, db, opMgmtNonceConsume, test.mutate(consumeFields()))
			var bumped bool
			for _, sql := range db.executed {
				if strings.Contains(sql, "UPDATE server_mgmt_status_hwm") {
					bumped = true
				}
			}
			if bumped != test.bump {
				t.Fatalf("advanced the mark = %v, want %v", bumped, test.bump)
			}
		})
	}
}

func TestConsumeRefusesAMalformedNonce(t *testing.T) {
	for _, test := range []struct{ name, nonce string }{
		{"too short", strings.Repeat("ab", 31)},
		{"too long", strings.Repeat("ab", 33)},
		{"uppercase", strings.ToUpper(goodNonce)},
		{"not hex", strings.Repeat("zz", 32)},
		{"empty", ""},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newNonceDB()
			db.challenge = storedChallenge()
			fields := consumeFields()
			fields[0] = test.nonce
			if got := nonceOutcome(t, db, opMgmtNonceConsume, fields); got != nonceInvalid {
				t.Fatalf("outcome = %d, want %d (invalid)", got, nonceInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("a malformed nonce ran %d statements", len(db.executed))
			}
		})
	}
}

// A delete that does not remove exactly one row means something else is writing
// this table, and the verdict is not safe to report.
func TestConsumeRefusesWhenTheSpendIsNotExactlyOneRow(t *testing.T) {
	for _, rows := range []int{0, 2} {
		db := newNonceDB()
		db.challenge = storedChallenge()
		db.deleteRows = rows
		if got := nonceOutcome(t, db, opMgmtNonceConsume, consumeFields()); got != nonceStorage {
			t.Fatalf("rows=%d outcome = %d, want %d (storage)", rows, got, nonceStorage)
		}
		if db.committed {
			t.Fatalf("rows=%d committed anyway", rows)
		}
	}
}

// --- issue -------------------------------------------------------------------

func issueFields() []string {
	return []string{goodNonce, "CN=issuer", "00ff", "fingerprint", "binding", "server-1",
		"attest", "1000"}
}

func TestIssueSweepsThenCountsThenInserts(t *testing.T) {
	db := newNonceDB()
	if got := nonceOutcome(t, db, opMgmtNonceIssue, issueFields()); got != nonceOK {
		t.Fatalf("outcome = %d, want ok", got)
	}
	if len(db.executed) < 3 {
		t.Fatalf("only %d statements ran: %v", len(db.executed), db.executed)
	}
	if !strings.Contains(db.executed[0], "DELETE FROM server_mgmt_nonce WHERE expires_at") {
		t.Fatalf("first statement was %q, want the sweep", db.executed[0])
	}
	if !strings.Contains(db.executed[1], "count(*)") {
		t.Fatalf("second statement was %q, want the count", db.executed[1])
	}
	if !strings.Contains(db.executed[2], "INSERT") {
		t.Fatalf("third statement was %q, want the insert", db.executed[2])
	}
}

// Saturation commits the sweep, for the same reason the jti stores do: a
// saturated store that discarded its own sweep could never drain.
func TestIssueSaturationCommitsTheSweepAndSkipsTheInsert(t *testing.T) {
	db := newNonceDB()
	db.liveCount = nonceCap
	if got := nonceOutcome(t, db, opMgmtNonceIssue, issueFields()); got != nonceSaturated {
		t.Fatalf("outcome = %d, want %d (saturated)", got, nonceSaturated)
	}
	if !db.committed {
		t.Fatalf("a saturated issue discarded its sweep")
	}
	for _, sql := range db.executed {
		if strings.Contains(sql, "INSERT") {
			t.Fatalf("a saturated issue still inserted")
		}
	}
}

// --- the high-water mark -----------------------------------------------------

// The mark only moves forward. A lower generation matches no row, and that is
// reported rather than quietly succeeding.
func TestHWMSetIsMonotonic(t *testing.T) {
	t.Run("forward succeeds", func(t *testing.T) {
		db := newNonceDB()
		status, _ := nonceCall(t, db, opMgmtStatusHWMSet, []string{"10"})
		if status != store.StatusOK {
			t.Fatalf("status = %d, want OK", status)
		}
		if !db.committed {
			t.Fatalf("a successful set did not commit")
		}
	})

	t.Run("backward is refused and rolls back", func(t *testing.T) {
		db := newNonceDB()
		db.hwmRows = 0 // the WHERE generation <= $1 matched nothing
		status, _ := nonceCall(t, db, opMgmtStatusHWMSet, []string{"1"})
		if status != store.StatusFailed {
			t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
		}
		if db.committed {
			t.Fatalf("a refused set committed")
		}
		if !db.rolledBack {
			t.Fatalf("a refused set did not roll back")
		}
	})

	t.Run("negative is invalid", func(t *testing.T) {
		db := newNonceDB()
		status, _ := nonceCall(t, db, opMgmtStatusHWMSet, []string{"-1"})
		if status != store.StatusInvalid {
			t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
		}
	})
}

// The seed row is part of the schema, so its absence is a broken store rather
// than a cold one -- reporting MISSING would invite a caller to treat an
// unknown revocation state as generation zero.
func TestHWMReadTreatsAMissingRowAsAFailure(t *testing.T) {
	db := newNonceDB()
	db.hwmMissing = true
	status, _ := nonceCall(t, db, opMgmtStatusHWMRead, nil)
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}

	db = newNonceDB()
	db.hwm = 42
	status, cells := nonceCall(t, db, opMgmtStatusHWMRead, nil)
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "42" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

func TestClearRemovesEverything(t *testing.T) {
	db := newNonceDB()
	status, cells := nonceCall(t, db, opMgmtNonceClear, nil)
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != 0 {
		t.Fatalf("clear carried %d cells, want 0", len(cells))
	}
	if len(db.executed) != 1 || !strings.Contains(db.executed[0], "DELETE FROM server_mgmt_nonce") {
		t.Fatalf("statements = %v", db.executed)
	}
	if !db.committed {
		t.Fatalf("clear did not commit")
	}
}

func TestNonceStorageErrorsAreReportedAsStorage(t *testing.T) {
	for _, test := range []struct {
		name string
		db   func() *nonceDB
	}{
		{"count fails", func() *nonceDB { d := newNonceDB(); d.countErr = errors.New("x"); return d }},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := test.db()
			if got := nonceOutcome(t, db, opMgmtNonceIssue, issueFields()); got != nonceStorage {
				t.Fatalf("outcome = %d, want %d (storage)", got, nonceStorage)
			}
		})
	}
}

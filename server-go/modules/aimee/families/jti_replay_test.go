package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a scripted database -----------------------------------------------------

type scriptedRow struct {
	count int64
	err   error
}

func (r scriptedRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) == 1 {
		if p, ok := dest[0].(*int64); ok {
			*p = r.count
		}
	}
	return nil
}

type scriptedTx struct {
	db *scriptedDB
}

func (t *scriptedTx) Exec(_ context.Context, sql string, _ ...any) (store.Tag, error) {
	t.db.executed = append(t.db.executed, sql)
	switch {
	case strings.HasPrefix(strings.TrimSpace(sql), "DELETE"):
		if t.db.gcErr != nil {
			return store.RowsAffected(0), t.db.gcErr
		}
		return store.RowsAffected(0), nil
	default:
		if t.db.insertErr != nil {
			return store.RowsAffected(0), t.db.insertErr
		}
		if t.db.insertConflict {
			return store.RowsAffected(0), nil
		}
		return store.RowsAffected(1), nil
	}
}

func (t *scriptedTx) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (t *scriptedTx) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	t.db.executed = append(t.db.executed, sql)
	return scriptedRow{count: t.db.liveCount, err: t.db.countErr}
}
func (t *scriptedTx) Commit(context.Context) error {
	t.db.committed = true
	return t.db.commitErr
}
func (t *scriptedTx) Rollback(context.Context) error {
	t.db.rolledBack = true
	return nil
}

type scriptedDB struct {
	liveCount      int64
	insertConflict bool
	beginErr       error
	gcErr          error
	countErr       error
	insertErr      error
	commitErr      error

	executed   []string
	committed  bool
	rolledBack bool
}

func (d *scriptedDB) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(0), errors.New("jti ops must run inside a transaction")
}
func (d *scriptedDB) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (d *scriptedDB) QueryRow(context.Context, string, ...any) store.Row {
	return scriptedRow{err: errors.New("jti ops must run inside a transaction")}
}
func (d *scriptedDB) Begin(context.Context) (store.Tx, error) {
	if d.beginErr != nil {
		return nil, d.beginErr
	}
	return &scriptedTx{db: d}, nil
}

// --- fixtures ----------------------------------------------------------------

// identityFields is a record that passes every validation rule, so a test can
// change exactly one thing and know that is what it measured.
func identityFields() []string {
	return []string{
		"abcd1234",             // jti, >= 8 chars
		"https://issuer.local", // issuer
		"key-1",                // kid
		"aimee.data",           // audience
		"user@example.com",     // subject
		"7",                    // team_id
		"data",                 // tier
		"1000",                 // issued_at
		"2000",                 // expires_at
		"1500",                 // consumed_at
	}
}

func managementFields() []string {
	hex64 := strings.Repeat("ab", 32)
	return []string{
		"abcd1234abcd1234",     // jti, >= 16 chars
		"https://issuer.local", // issuer
		"key-1",                // kid
		"aimee.mgmt",           // audience
		"admin@example.com",    // subject
		"7",                    // team_id
		"deploy",               // capability
		"CN=peer",              // peer_issuer
		"00ff",                 // peer_serial, lowercase hex
		hex64,                  // peer_fingerprint
		hex64,                  // request_sha256
		"corr-1",               // correlation_id
		"1000",                 // issued_at
		"2000",                 // expires_at
		"1500",                 // consumed_at
	}
}

// call drives the family the way the bus does and returns the single result
// cell every consume answers with.
func call(t *testing.T, db store.DB, op uint32, fields []string) int {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := JTIReplay.Handler(db)(
		bus.ModuleInvocation{StageID: StageJTIReplay}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK -- the C client maps any other status to -1, "+
			"which is not one of the result codes", status)
	}
	wireStatus, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if wireStatus != store.StatusOK {
		t.Fatalf("in-band status = %d, want OK: every outcome rides in the result cell", wireStatus)
	}
	if len(cells) != 1 {
		t.Fatalf("reply carried %d cells, want 1", len(cells))
	}
	value, ok := store.Atoi(cells[0])
	if !ok {
		t.Fatalf("result cell %q is not a number", cells[0])
	}
	return value
}

// --- the decision table ------------------------------------------------------

// This is the reason these operations manage their own transaction. Saturation
// must KEEP the garbage-collection pass that just ran; a replay must discard
// everything about the attempt.
func TestConsumeCommitsOrRollsBackPerOutcome(t *testing.T) {
	tests := []struct {
		name     string
		db       *scriptedDB
		want     int
		commit   bool
		rollback bool
	}{
		{
			name:   "fresh jti commits",
			db:     &scriptedDB{liveCount: 10},
			want:   jtiOK,
			commit: true,
		},
		{
			// The sweep is real work. Discarding it would mean a saturated
			// store sweeps and throws the sweep away on every single consume,
			// so it could never drain back below the limit.
			name:   "saturated commits the sweep",
			db:     &scriptedDB{liveCount: jtiLiveLimit},
			want:   jtiSaturated,
			commit: true,
		},
		{
			name:     "replay rolls back",
			db:       &scriptedDB{liveCount: 10, insertConflict: true},
			want:     jtiReplay,
			rollback: true,
		},
		{
			name:     "gc failure rolls back",
			db:       &scriptedDB{liveCount: 10, gcErr: errors.New("gc broke")},
			want:     jtiStorage,
			rollback: true,
		},
		{
			name:     "count failure rolls back",
			db:       &scriptedDB{liveCount: 10, countErr: errors.New("count broke")},
			want:     jtiStorage,
			rollback: true,
		},
		{
			name:     "insert failure rolls back",
			db:       &scriptedDB{liveCount: 10, insertErr: errors.New("insert broke")},
			want:     jtiStorage,
			rollback: true,
		},
		{
			name: "begin failure is a storage error",
			db:   &scriptedDB{beginErr: errors.New("no connection")},
			want: jtiStorage,
		},
		{
			name:   "lost commit is a storage error, not a success",
			db:     &scriptedDB{liveCount: 10, commitErr: errors.New("commit lost")},
			want:   jtiStorage,
			commit: true,
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got := call(t, test.db, opIdentityJTIConsume, identityFields())
			if got != test.want {
				t.Fatalf("result = %d, want %d", got, test.want)
			}
			if test.db.committed != test.commit {
				t.Fatalf("committed = %v, want %v", test.db.committed, test.commit)
			}
			if test.db.rolledBack != test.rollback {
				t.Fatalf("rolledBack = %v, want %v", test.db.rolledBack, test.rollback)
			}
		})
	}
}

// Saturation is checked BEFORE the insert. Checking after would mean the row
// that tips the store over its limit is the one that gets written.
func TestSaturationIsCheckedBeforeInserting(t *testing.T) {
	db := &scriptedDB{liveCount: jtiLiveLimit}
	if got := call(t, db, opIdentityJTIConsume, identityFields()); got != jtiSaturated {
		t.Fatalf("result = %d, want saturated", got)
	}
	for _, sql := range db.executed {
		if strings.Contains(sql, "INSERT") {
			t.Fatalf("a saturated store still ran an INSERT")
		}
	}
}

// The sweep must run before the count, or the count includes rows the sweep was
// about to remove and a store that is merely stale reads as saturated.
func TestSweepRunsBeforeTheCount(t *testing.T) {
	db := &scriptedDB{liveCount: 10}
	call(t, db, opIdentityJTIConsume, identityFields())
	if len(db.executed) < 2 {
		t.Fatalf("only %d statements ran", len(db.executed))
	}
	if !strings.HasPrefix(strings.TrimSpace(db.executed[0]), "DELETE") {
		t.Fatalf("first statement was %q, want the sweep", db.executed[0])
	}
	if !strings.Contains(db.executed[1], "count(*)") {
		t.Fatalf("second statement was %q, want the count", db.executed[1])
	}
}

// --- validation --------------------------------------------------------------

// A record that fails validation is INVALID and must never reach the database:
// the C refused before opening its transaction and so does this.
func TestInvalidRecordsAreRefusedWithoutTouchingTheStore(t *testing.T) {
	tests := []struct {
		name  string
		index int
		value string
	}{
		{"jti too short", 0, "short"},
		{"jti bad alphabet", 0, "has spaces!"},
		{"issuer empty", 1, ""},
		{"issuer has a control character", 1, "iss\x01uer"},
		{"kid empty", 2, ""},
		{"audience bad alphabet", 3, "aud ience"},
		{"subject empty", 4, ""},
		{"team_id zero", 5, "0"},
		{"team_id negative", 5, "-1"},
		{"team_id not a number", 5, "seven"},
		{"tier unrecognised", 6, "admin"},
		{"issued_at negative", 7, "-1"},
		{"expires_at before issued_at", 8, "500"},
		{"consumed_at before issued_at", 9, "999"},
		{"consumed_at at expiry", 9, "2000"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			fields := identityFields()
			fields[test.index] = test.value
			db := &scriptedDB{liveCount: 10}
			if got := call(t, db, opIdentityJTIConsume, fields); got != jtiInvalid {
				t.Fatalf("result = %d, want %d (invalid)", got, jtiInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid record ran %d statements", len(db.executed))
			}
			if db.committed || db.rolledBack {
				t.Fatalf("an invalid record opened a transaction")
			}
		})
	}
}

// The management token's rules differ from the identity token's in more than
// its length floor, and each difference is load-bearing.
func TestManagementValidationDiffersFromIdentity(t *testing.T) {
	tests := []struct {
		name  string
		index int
		value string
	}{
		{"jti of 8 is too short here", 0, "abcd1234"},
		{"capability empty", 6, ""},
		{"peer_issuer empty", 7, ""},
		{"peer_serial uppercase hex", 8, "00FF"},
		{"peer_serial not hex", 8, "zz"},
		{"fingerprint wrong length", 9, strings.Repeat("ab", 16)},
		{"fingerprint uppercase", 9, strings.Repeat("AB", 32)},
		{"request_sha256 wrong length", 10, "abc"},
		{"correlation_id bad alphabet", 11, "corr id"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			fields := managementFields()
			fields[test.index] = test.value
			db := &scriptedDB{liveCount: 10}
			if got := call(t, db, opManagementJTIConsume, fields); got != jtiInvalid {
				t.Fatalf("result = %d, want %d (invalid)", got, jtiInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid record ran %d statements", len(db.executed))
			}
		})
	}

	// The same record, unmodified, must pass -- otherwise the table above is
	// measuring a broken fixture rather than the rules.
	db := &scriptedDB{liveCount: 10}
	if got := call(t, db, opManagementJTIConsume, managementFields()); got != jtiOK {
		t.Fatalf("the valid management fixture returned %d, want ok", got)
	}
}

// An identity jti of exactly 8 is legal; the management floor is 16. Both
// boundaries are checked because an off-by-one either way changes which tokens
// the server will accept.
func TestJTILengthFloorsAreExact(t *testing.T) {
	for _, test := range []struct {
		name string
		op   uint32
		base func() []string
		jti  string
		want int
	}{
		{"identity 7 rejected", opIdentityJTIConsume, identityFields, strings.Repeat("a", 7), jtiInvalid},
		{"identity 8 accepted", opIdentityJTIConsume, identityFields, strings.Repeat("a", 8), jtiOK},
		{"identity 128 accepted", opIdentityJTIConsume, identityFields, strings.Repeat("a", 128), jtiOK},
		{"identity 129 rejected", opIdentityJTIConsume, identityFields, strings.Repeat("a", 129), jtiInvalid},
		{"management 15 rejected", opManagementJTIConsume, managementFields, strings.Repeat("a", 15), jtiInvalid},
		{"management 16 accepted", opManagementJTIConsume, managementFields, strings.Repeat("a", 16), jtiOK},
	} {
		t.Run(test.name, func(t *testing.T) {
			fields := test.base()
			fields[0] = test.jti
			if got := call(t, &scriptedDB{liveCount: 10}, test.op, fields); got != test.want {
				t.Fatalf("result = %d, want %d", got, test.want)
			}
		})
	}
}

// Arity is enforced by the postgres module's shared layer from the catalog, and a frame with the wrong
// field count must not reach an operation that would index past its end.
func TestWrongFieldCountIsRefused(t *testing.T) {
	for _, test := range []struct {
		name string
		op   uint32
		n    int
	}{
		{"identity too few", opIdentityJTIConsume, 9},
		{"identity too many", opIdentityJTIConsume, 11},
		{"management too few", opManagementJTIConsume, 14},
	} {
		t.Run(test.name, func(t *testing.T) {
			frame, _ := wire.EncodeFields(test.op, make([]string, test.n))
			response, status := JTIReplay.Handler(&scriptedDB{})(
				bus.ModuleInvocation{StageID: StageJTIReplay}, frame)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if got, _, _ := wire.DecodeFields(response); got != store.StatusInvalid {
				t.Fatalf("in-band status = %d, want invalid", got)
			}
		})
	}
}

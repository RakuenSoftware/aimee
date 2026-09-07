package families

import (
	"context"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a grant-store-shaped fake -----------------------------------------------

type grant struct {
	principal string
	bearer    string
	serial    string
	tier      string
}

type identRow struct {
	db  *identDB
	sql string
}

func (r identRow) Scan(dest ...any) error {
	switch {
	case strings.Contains(r.sql, "FROM remote_first_user"):
		if r.db.owner == "" {
			return store.ErrNoRows
		}
		*(dest[0].(*string)) = r.db.owner
		return nil
	case strings.Contains(r.sql, "coalesce(cert_serial, '') FROM remote_client_grants"):
		if r.db.grant == nil {
			return store.ErrNoRows
		}
		*(dest[0].(*string)) = r.db.grant.serial
		return nil
	case strings.Contains(r.sql, "FROM remote_client_grants\n\t                         WHERE principal"),
		strings.Contains(r.sql, "WHERE principal = $1"):
		if r.db.grant == nil {
			return store.ErrNoRows
		}
		g := r.db.grant
		*(dest[0].(*string)) = g.principal
		*(dest[1].(*string)) = g.bearer
		*(dest[2].(*string)) = g.serial
		*(dest[3].(*string)) = g.tier
		return nil
	default: // tier lookup by cert serial
		if r.db.grant == nil || r.db.grant.serial == "" {
			return store.ErrNoRows
		}
		*(dest[0].(*string)) = r.db.grant.principal
		*(dest[1].(*string)) = r.db.grant.tier
		return nil
	}
}

type identTx struct{ db *identDB }

func (t *identTx) Exec(_ context.Context, sql string, _ ...any) (store.Tag, error) {
	t.db.executed = append(t.db.executed, sql)
	if strings.Contains(sql, "UPDATE remote_client_grants") {
		return store.RowsAffected(t.db.bindRows), nil
	}
	return store.RowsAffected(1), nil
}
func (t *identTx) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (t *identTx) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	t.db.executed = append(t.db.executed, sql)
	return identRow{db: t.db, sql: sql}
}
func (t *identTx) Commit(context.Context) error   { t.db.committed = true; return nil }
func (t *identTx) Rollback(context.Context) error { t.db.rolledBack = true; return nil }

type identDB struct {
	owner    string
	grant    *grant
	bindRows int

	executed   []string
	committed  bool
	rolledBack bool
}

func newIdentDB() *identDB { return &identDB{bindRows: 1} }

func (d *identDB) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	return (&identTx{db: d}).Exec(ctx, sql, args...)
}
func (d *identDB) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (d *identDB) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	d.executed = append(d.executed, sql)
	return identRow{db: d, sql: sql}
}
func (d *identDB) Begin(context.Context) (store.Tx, error) { return &identTx{db: d}, nil }

// Contains hex LETTERS on purpose: an all-digit fixture is unchanged by
// strings.ToUpper, so it cannot test that the case check works.
const testBearer = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"

func identCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := Identity.Handler(db)(
		bus.ModuleInvocation{StageID: StageIdentity}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

func claimCells(t *testing.T, db store.DB, principal, bearer, now string) []string {
	t.Helper()
	status, cells := identCall(t, db, opRemoteClientClaim, []string{principal, bearer, now})
	if status != store.StatusOK {
		t.Fatalf("in-band status = %d, want OK", status)
	}
	if len(cells) != 5 {
		t.Fatalf("claim reply carried %d cells, want 5", len(cells))
	}
	return cells
}

func wrote(db *identDB, fragment string) bool {
	for _, sql := range db.executed {
		if strings.Contains(sql, fragment) {
			return true
		}
	}
	return false
}

// --- the first-user protection -----------------------------------------------

// This is the whole point of the family: the first principal to claim owns the
// server, and a different one afterwards gets told so rather than being handed
// a credential.
func TestFirstClaimTakesOwnershipAndASecondPrincipalIsRefused(t *testing.T) {
	db := newIdentDB()
	cells := claimCells(t, db, "webuser:alice", testBearer, "1000")
	if cells[0] != store.Itoa(claimNew) {
		t.Fatalf("first claim = %s, want %d (new)", cells[0], claimNew)
	}
	if !wrote(db, "INSERT INTO remote_first_user") {
		t.Fatalf("the first claim did not record an owner")
	}
	if !wrote(db, "INSERT INTO remote_client_grants") {
		t.Fatalf("the first claim did not mint a grant")
	}
	if cells[1] != "webuser:alice" || cells[2] != testBearer || cells[3] != "" {
		t.Fatalf("grant cells = %v", cells[1:])
	}
	if cells[4] != store.Itoa(2) {
		t.Fatalf("a fresh grant's tier = %s, want 2 (full)", cells[4])
	}

	other := newIdentDB()
	other.owner = "webuser:alice"
	cells = claimCells(t, other, "webuser:mallory", testBearer, "1000")
	if cells[0] != store.Itoa(claimOwnedByOther) {
		t.Fatalf("second principal = %s, want %d (owned by other)", cells[0], claimOwnedByOther)
	}
	// The refusal must not leak the owner's identity or credential back to the
	// caller who was turned away.
	for i, cell := range cells[1:4] {
		if cell != "" {
			t.Fatalf("a refused claim returned cell %d = %q", i+1, cell)
		}
	}
	if wrote(other, "INSERT INTO remote_client_grants") {
		t.Fatalf("a refused claim minted a grant anyway")
	}
}

// Re-claiming as the owner recovers the SAME grant rather than minting another:
// re-running Deploy must not leave a second standing credential behind.
func TestReclaimingRecoversTheExistingGrant(t *testing.T) {
	t.Run("unbound", func(t *testing.T) {
		db := newIdentDB()
		db.owner = "webuser:alice"
		db.grant = &grant{principal: "webuser:alice", bearer: testBearer, serial: "", tier: "full"}
		cells := claimCells(t, db, "webuser:alice", strings.Repeat("2", 64), "2000")
		if cells[0] != store.Itoa(claimUnbound) {
			t.Fatalf("result = %s, want %d (unbound)", cells[0], claimUnbound)
		}
		if cells[2] != testBearer {
			t.Fatalf("returned bearer %q, want the stored one -- a new one was minted", cells[2])
		}
		if wrote(db, "INSERT INTO remote_client_grants") {
			t.Fatalf("re-claiming minted a second grant")
		}
	})

	t.Run("bound", func(t *testing.T) {
		db := newIdentDB()
		db.owner = "webuser:alice"
		db.grant = &grant{principal: "webuser:alice", bearer: testBearer, serial: "ABCD", tier: "full"}
		cells := claimCells(t, db, "webuser:alice", strings.Repeat("2", 64), "2000")
		if cells[0] != store.Itoa(claimBound) {
			t.Fatalf("result = %s, want %d (bound)", cells[0], claimBound)
		}
		if cells[3] != "ABCD" {
			t.Fatalf("cert serial = %q, want ABCD", cells[3])
		}
	})
}

func TestClaimRefusesMalformedInputWithoutTouchingTheStore(t *testing.T) {
	for _, test := range []struct {
		name                   string
		principal, bearer, now string
	}{
		{"principal is not a webuser", "service:robot", testBearer, "1000"},
		{"principal too short", "webuser:", testBearer, "1000"},
		{"principal has a control character", "webuser:a\x01b", testBearer, "1000"},
		{"bearer not 64 hex", "webuser:alice", "abcd", "1000"},
		{"bearer uppercase", "webuser:alice", strings.ToUpper(testBearer), "1000"},
		{"now negative", "webuser:alice", testBearer, "-1"},
		{"now not a number", "webuser:alice", testBearer, "later"},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newIdentDB()
			cells := claimCells(t, db, test.principal, test.bearer, test.now)
			if cells[0] != store.Itoa(claimInvalid) {
				t.Fatalf("result = %s, want %d (invalid)", cells[0], claimInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("a malformed claim ran %d statements", len(db.executed))
			}
		})
	}
}

// --- binding -----------------------------------------------------------------

// Re-binding the same certificate is a retry and succeeds. Binding a different
// one would transfer the principal's authority to another key, so it is
// refused and nothing is written.
func TestBindAcceptsARetryButRefusesATransfer(t *testing.T) {
	t.Run("fresh bind writes", func(t *testing.T) {
		db := newIdentDB()
		db.grant = &grant{bearer: testBearer, serial: ""}
		_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "ABCD", "1000"})
		if cells[0] != store.Itoa(bindOK) {
			t.Fatalf("result = %s, want %d", cells[0], bindOK)
		}
		if !wrote(db, "UPDATE remote_client_grants") {
			t.Fatalf("a fresh bind did not update")
		}
	})

	t.Run("same certificate is a retry", func(t *testing.T) {
		db := newIdentDB()
		db.grant = &grant{bearer: testBearer, serial: "ABCD"}
		_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "ABCD", "1000"})
		if cells[0] != store.Itoa(bindOK) {
			t.Fatalf("result = %s, want %d", cells[0], bindOK)
		}
		if wrote(db, "UPDATE remote_client_grants") {
			t.Fatalf("a retried bind rewrote the pairing")
		}
	})

	t.Run("different certificate is refused", func(t *testing.T) {
		db := newIdentDB()
		db.grant = &grant{bearer: testBearer, serial: "ABCD"}
		_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "BEEF", "1000"})
		if cells[0] != store.Itoa(bindOtherCert) {
			t.Fatalf("result = %s, want %d (already bound elsewhere)", cells[0], bindOtherCert)
		}
		if wrote(db, "UPDATE remote_client_grants") {
			t.Fatalf("a transfer was written")
		}
	})

	t.Run("no such grant", func(t *testing.T) {
		db := newIdentDB()
		_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "ABCD", "1000"})
		if cells[0] != store.Itoa(bindNoSuchGrant) {
			t.Fatalf("result = %s, want %d", cells[0], bindNoSuchGrant)
		}
	})

	// A concurrent writer taking the row between the read and the update must
	// not be reported as a successful pairing.
	t.Run("update matching no row is a failure", func(t *testing.T) {
		db := newIdentDB()
		db.grant = &grant{bearer: testBearer, serial: ""}
		db.bindRows = 0
		_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "ABCD", "1000"})
		if cells[0] != store.Itoa(bindFailed) {
			t.Fatalf("result = %s, want %d (failed)", cells[0], bindFailed)
		}
		if db.committed {
			t.Fatalf("a failed bind committed")
		}
	})
}

func TestBindRefusesMalformedInput(t *testing.T) {
	for _, test := range []struct{ name, bearer, serial, now string }{
		{"bearer not hex", "zz", "ABCD", "1000"},
		{"serial empty", testBearer, "", "1000"},
		{"serial not hex", testBearer, "GHIJ", "1000"},
		{"serial too long", testBearer, strings.Repeat("a", certSerialMax+1), "1000"},
		{"now negative", testBearer, "ABCD", "-1"},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newIdentDB()
			_, cells := identCall(t, db, opRemoteClientBind,
				[]string{test.bearer, test.serial, test.now})
			if cells[0] != store.Itoa(bindFailed) {
				t.Fatalf("result = %s, want %d", cells[0], bindFailed)
			}
			if len(db.executed) != 0 {
				t.Fatalf("a malformed bind ran %d statements", len(db.executed))
			}
		})
	}
}

// --- tier and abandon ---------------------------------------------------------

// An unknown certificate answers with an empty principal, which IS the "no such
// grant" answer -- so it must never come back with someone else's principal.
func TestTierAnswersEmptyForAnUnknownCertificate(t *testing.T) {
	db := newIdentDB()
	status, cells := identCall(t, db, opRemoteClientTier, []string{"ABCD"})
	if status != store.StatusOK || len(cells) != 2 {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if cells[0] != "" || cells[1] != "0" {
		t.Fatalf("unknown certificate = %v, want [\"\" \"0\"]", cells)
	}

	db = newIdentDB()
	db.grant = &grant{principal: "webuser:alice", serial: "ABCD", tier: "full"}
	_, cells = identCall(t, db, opRemoteClientTier, []string{"ABCD"})
	if cells[0] != "webuser:alice" || cells[1] != "2" {
		t.Fatalf("known certificate = %v", cells)
	}

	db = newIdentDB()
	db.grant = &grant{principal: "webuser:bob", serial: "ABCD", tier: "data"}
	_, cells = identCall(t, db, opRemoteClientTier, []string{"ABCD"})
	if cells[1] != "1" {
		t.Fatalf("data tier ranked %s, want 1", cells[1])
	}
}

func TestTierRefusesAMalformedSerialWithoutQuerying(t *testing.T) {
	db := newIdentDB()
	_, cells := identCall(t, db, opRemoteClientTier, []string{"not-hex"})
	if cells[0] != "" || cells[1] != "-1" {
		t.Fatalf("cells = %v, want an empty principal and -1", cells)
	}
	if len(db.executed) != 0 {
		t.Fatalf("a malformed serial was queried anyway")
	}
}

// Abandon only removes an enrollment that was never completed; the WHERE clause
// is what protects a bound grant, so the statement must carry it.
func TestAbandonOnlyTargetsUnboundGrants(t *testing.T) {
	db := newIdentDB()
	status, _ := identCall(t, db, opRemoteClientAbandon, []string{testBearer})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(db.executed) != 1 {
		t.Fatalf("statements = %v", db.executed)
	}
	if !strings.Contains(db.executed[0], "cert_serial IS NULL") {
		t.Fatalf("abandon ran %q -- it would delete a bound grant", db.executed[0])
	}
	if !db.committed {
		t.Fatalf("abandon did not commit")
	}
}

func TestAbandonRefusesAMalformedBearer(t *testing.T) {
	db := newIdentDB()
	status, _ := identCall(t, db, opRemoteClientAbandon, []string{"abcd"})
	if status != store.StatusInvalid {
		t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
	}
	if len(db.executed) != 0 {
		t.Fatalf("a malformed bearer ran %d statements", len(db.executed))
	}
}

package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a PKI-shaped fake --------------------------------------------------------

type cert struct {
	serial    string
	cn        string
	issued    int64
	expires   int64
	presented int64
	revoked   bool
}

type rampRow struct {
	state      int
	hash       string
	advancedAt int64
	absent     bool
}

type pkiRow struct {
	db  *pkiDB
	sql string
}

func (r pkiRow) Scan(dest ...any) error {
	if r.db.scanErr != nil {
		return r.db.scanErr
	}
	switch {
	case strings.Contains(r.sql, "extract(epoch"):
		*(dest[0].(*int64)) = r.db.now
		return nil
	case strings.Contains(r.sql, "FROM pki_mtls_ramp"):
		if r.db.ramp.absent {
			return store.ErrNoRows
		}
		*(dest[0].(*int)) = r.db.ramp.state
		*(dest[1].(*string)) = r.db.ramp.hash
		*(dest[2].(*int64)) = r.db.ramp.advancedAt
		return nil
	default: // cert check
		for _, c := range r.db.certs {
			if c.serial == r.db.lookup {
				*(dest[0].(*bool)) = c.revoked
				*(dest[1].(*int64)) = c.expires
				return nil
			}
		}
		return store.ErrNoRows
	}
}

type pkiRows struct {
	rows [][]any
	at   int
}

func (r *pkiRows) Close()     {}
func (r *pkiRows) Err() error { return nil }
func (r *pkiRows) Next() bool {
	if r.at >= len(r.rows) {
		return false
	}
	r.at++
	return true
}
func (r *pkiRows) Scan(dest ...any) error {
	row := r.rows[r.at-1]
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = row[i].(string)
		case *int64:
			*p = row[i].(int64)
		case *bool:
			*p = row[i].(bool)
		}
	}
	return nil
}

type pkiDB struct {
	certs    []cert
	ramp     rampRow
	now      int64
	lookup   string
	scanErr  error
	execRows int

	executed []string
	args     [][]any
}

func newPKIDB() *pkiDB {
	return &pkiDB{ramp: rampRow{state: rampObserve, hash: strings.Repeat("0", 64)}, execRows: 1}
}

// liveRoster is what pkiRosterSQL would return: unrevoked and unexpired, in
// (serial, cn) order.
func (d *pkiDB) liveRoster(now int64) [][]any {
	var out [][]any
	for _, c := range d.certs {
		if c.revoked || (c.expires != 0 && c.expires <= now) {
			continue
		}
		out = append(out, []any{c.serial, c.cn, c.issued, c.expires, c.presented})
	}
	return out
}

func (d *pkiDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return store.RowsAffected(d.execRows), nil
}
func (d *pkiDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	switch {
	case strings.Contains(sql, "last_presented_at\n"), strings.Contains(sql, "last_presented_at "):
		now := int64(0)
		if len(args) > 0 {
			now, _ = args[0].(int64)
		}
		return &pkiRows{rows: d.liveRoster(now)}, nil
	case strings.Contains(sql, "WHERE revoked"):
		var out [][]any
		for _, c := range d.certs {
			if c.revoked {
				out = append(out, []any{c.serial})
			}
		}
		return &pkiRows{rows: out}, nil
	default: // cert list
		var out [][]any
		for _, c := range d.certs {
			out = append(out, []any{c.serial, c.cn, c.issued, c.expires, c.revoked})
		}
		return &pkiRows{rows: out}, nil
	}
}
func (d *pkiDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	if len(args) > 0 {
		if s, ok := args[0].(string); ok {
			d.lookup = s
		}
	}
	return pkiRow{db: d, sql: sql}
}
func (d *pkiDB) Begin(context.Context) (store.Tx, error) { return pkiTx{d}, nil }

type pkiTx struct{ *pkiDB }

func (t pkiTx) Commit(context.Context) error   { return nil }
func (t pkiTx) Rollback(context.Context) error { return nil }

func pkiCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := PKI.Handler(db)(bus.ModuleInvocation{StageID: StagePKI}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// expectedHash reproduces the C's digest independently of the module's own
// helper, so the two can disagree.
func expectedHash(certs []cert, now int64) string {
	h := sha256.New()
	for _, c := range certs {
		if c.revoked || (c.expires != 0 && c.expires <= now) {
			continue
		}
		h.Write([]byte(c.serial))
		h.Write([]byte{0})
		h.Write([]byte(c.cn))
		h.Write([]byte{0})
		h.Write([]byte(fmt.Sprintf("%d:%d", c.issued, c.expires)))
		h.Write([]byte{0})
	}
	return hex.EncodeToString(h.Sum(nil))
}

// --- the certificate verdict --------------------------------------------------

// Every path fails closed, and the four refusals stay distinct: an operator
// needs to know whether a certificate was revoked, expired, never on the
// roster, or whether the roster itself could not be read.
func TestCertCheckFailsClosedAndKeepsItsReasons(t *testing.T) {
	live := cert{serial: "AA", cn: "node", issued: 100, expires: 5000}
	for _, test := range []struct {
		name   string
		db     func() *pkiDB
		serial string
		now    string
		want   int
	}{
		{"valid", func() *pkiDB { d := newPKIDB(); d.certs = []cert{live}; return d }, "AA", "1000", certValid},
		{"revoked", func() *pkiDB {
			d := newPKIDB()
			c := live
			c.revoked = true
			d.certs = []cert{c}
			return d
		}, "AA", "1000", certRevoked},
		{"expired", func() *pkiDB { d := newPKIDB(); d.certs = []cert{live}; return d }, "AA", "9000", certExpired},
		{"unknown serial", func() *pkiDB { return newPKIDB() }, "ZZ", "1000", certUnknown},
		{"empty serial", func() *pkiDB { return newPKIDB() }, "", "1000", certUnknown},
		{"unreadable roster", func() *pkiDB {
			d := newPKIDB()
			d.certs = []cert{live}
			d.scanErr = errors.New("store broke")
			return d
		}, "AA", "1000", certError},
	} {
		t.Run(test.name, func(t *testing.T) {
			status, cells := pkiCall(t, test.db(), opPKICertCheck, []string{test.serial, test.now})
			if status != store.StatusOK || len(cells) != 1 {
				t.Fatalf("status = %d, cells = %v", status, cells)
			}
			got, _ := store.Atoi(cells[0])
			if got != test.want {
				t.Fatalf("verdict = %d, want %d", got, test.want)
			}
		})
	}
}

// A certificate expiring exactly now is expired: the C used <= and the
// boundary is what decides whether a renewal window has closed.
func TestExpiryBoundaryIsInclusive(t *testing.T) {
	db := newPKIDB()
	db.certs = []cert{{serial: "AA", cn: "node", issued: 100, expires: 1000}}
	_, cells := pkiCall(t, db, opPKICertCheck, []string{"AA", "1000"})
	if got, _ := store.Atoi(cells[0]); got != certExpired {
		t.Fatalf("verdict at the expiry instant = %d, want %d", got, certExpired)
	}
	_, cells = pkiCall(t, db, opPKICertCheck, []string{"AA", "999"})
	if got, _ := store.Atoi(cells[0]); got != certValid {
		t.Fatalf("verdict a second before expiry = %d, want %d", got, certValid)
	}
	// expires_at of 0 means "never", not "expired at the epoch".
	db.certs = []cert{{serial: "BB", cn: "node", issued: 100, expires: 0}}
	_, cells = pkiCall(t, db, opPKICertCheck, []string{"BB", "999999"})
	if got, _ := store.Atoi(cells[0]); got != certValid {
		t.Fatalf("a never-expiring certificate = %d, want %d", got, certValid)
	}
}

// --- the roster hash ----------------------------------------------------------

// The digest is stored and compared against what the C wrote, so its exact
// shape is load-bearing: sha256 over serial NUL cn NUL issued:expires NUL, live
// certificates only, in (serial, cn) order.
func TestRosterHashMatchesTheDigestShape(t *testing.T) {
	certs := []cert{
		{serial: "AA", cn: "a", issued: 10, expires: 5000, presented: 20},
		{serial: "BB", cn: "b", issued: 30, expires: 0, presented: 40},
		{serial: "CC", cn: "c", issued: 50, expires: 100, presented: 60}, // expired at now=1000
		{serial: "DD", cn: "d", issued: 70, expires: 5000, presented: 80, revoked: true},
	}
	db := newPKIDB()
	db.certs = certs
	got, err := snapshotRoster(context.Background(), db, 1000)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	if want := expectedHash(certs, 1000); got.hash != want {
		t.Fatalf("hash = %s, want %s", got.hash, want)
	}
	if got.count != 2 {
		t.Fatalf("counted %d live certificates, want 2", got.count)
	}
	if !got.ready {
		t.Fatalf("every live certificate has been presented, so the roster is ready")
	}
}

// A certificate nobody has presented since it was issued means the fleet has
// not picked up its material, so enforcement is not safe.
func TestRosterIsNotReadyUntilEveryCertificateHasBeenPresented(t *testing.T) {
	for _, test := range []struct {
		name      string
		presented int64
		ready     bool
	}{
		{"never presented", 0, false},
		{"presented before issue", 5, false},
		{"presented at issue", 10, true},
		{"presented after issue", 50, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newPKIDB()
			db.certs = []cert{{serial: "AA", cn: "a", issued: 10, expires: 5000, presented: test.presented}}
			got, err := snapshotRoster(context.Background(), db, 1000)
			if err != nil {
				t.Fatalf("snapshot: %v", err)
			}
			if got.ready != test.ready {
				t.Fatalf("ready = %v, want %v", got.ready, test.ready)
			}
		})
	}
}

// An empty roster is never ready. Enforcing against nothing would lock every
// client out with no certificate able to satisfy the check.
func TestAnEmptyRosterIsNeverReady(t *testing.T) {
	got, err := snapshotRoster(context.Background(), newPKIDB(), 1000)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	if got.ready || got.count != 0 {
		t.Fatalf("empty roster: ready = %v, count = %d", got.ready, got.count)
	}
}

// --- the ramp -----------------------------------------------------------------

func readyDB() *pkiDB {
	db := newPKIDB()
	db.certs = []cert{{serial: "AA", cn: "a", issued: 10, expires: 5000, presented: 20}}
	db.ramp = rampRow{state: rampObserve, hash: expectedHash(db.certs, 1000)}
	return db
}

func TestRampAdvancesOnlyWhenTheRosterIsReadyAndUnchanged(t *testing.T) {
	t.Run("ready and matching advances", func(t *testing.T) {
		db := readyDB()
		_, cells := pkiCall(t, db, opPKIRampAdvance, []string{"1000"})
		if cells[0] != "1" {
			t.Fatalf("advanced = %s, want 1", cells[0])
		}
		var advanced bool
		for _, sql := range db.executed {
			if strings.Contains(sql, "SET ramp_state = 2, last_advance_ts") {
				advanced = true
			}
		}
		if !advanced {
			t.Fatalf("no advance statement ran")
		}
	})

	// The stored hash no longer describing the roster means the fleet changed
	// since readiness was judged, so the judgement is about a fleet that no
	// longer exists.
	t.Run("stale hash does not advance", func(t *testing.T) {
		db := readyDB()
		db.ramp.hash = strings.Repeat("f", 64)
		_, cells := pkiCall(t, db, opPKIRampAdvance, []string{"1000"})
		if cells[0] != "0" {
			t.Fatalf("advanced = %s, want 0", cells[0])
		}
	})

	t.Run("unpresented certificate does not advance", func(t *testing.T) {
		db := newPKIDB()
		db.certs = []cert{{serial: "AA", cn: "a", issued: 10, expires: 5000, presented: 0}}
		db.ramp = rampRow{state: rampObserve, hash: expectedHash(db.certs, 1000)}
		_, cells := pkiCall(t, db, opPKIRampAdvance, []string{"1000"})
		if cells[0] != "0" {
			t.Fatalf("advanced = %s, want 0", cells[0])
		}
	})

	t.Run("empty roster does not advance", func(t *testing.T) {
		db := newPKIDB()
		db.ramp = rampRow{state: rampObserve, hash: expectedHash(nil, 1000)}
		_, cells := pkiCall(t, db, opPKIRampAdvance, []string{"1000"})
		if cells[0] != "0" {
			t.Fatalf("advanced = %s, want 0", cells[0])
		}
	})

	// Already enforcing is reported as not-ready: the question is whether there
	// is an advance to make, and there is not.
	t.Run("already enforcing reports nothing to do", func(t *testing.T) {
		db := readyDB()
		db.ramp.state = rampEnforce
		_, cells := pkiCall(t, db, opPKIRampReady, []string{"1000"})
		if cells[0] != "0" {
			t.Fatalf("ready = %s, want 0", cells[0])
		}
	})

	// A losing race is not an error: someone else advanced, or the roster moved
	// under the write.
	t.Run("a losing race is reported as not advanced", func(t *testing.T) {
		db := readyDB()
		db.execRows = 0
		status, cells := pkiCall(t, db, opPKIRampAdvance, []string{"1000"})
		if status != store.StatusOK || cells[0] != "0" {
			t.Fatalf("status = %d, cells = %v", status, cells)
		}
	})
}

// ready must not advance anything. It answers a question.
func TestRampReadyDoesNotAdvance(t *testing.T) {
	db := readyDB()
	_, cells := pkiCall(t, db, opPKIRampReady, []string{"1000"})
	if cells[0] != "1" {
		t.Fatalf("ready = %s, want 1", cells[0])
	}
	for _, sql := range db.executed {
		if strings.Contains(sql, "SET ramp_state = 2") {
			t.Fatalf("a readiness check advanced the ramp")
		}
	}
}

// Configuration moves the ramp forward only. Demoting an enforcing ramp back to
// observe would silently reopen a door the operator had closed.
func TestRampInitOnlyEverMovesForward(t *testing.T) {
	t.Run("configured to enforce promotes an observing ramp", func(t *testing.T) {
		db := readyDB()
		db.now = 2000
		_, cells := pkiCall(t, db, opPKIRampInit, []string{"2"})
		if cells[0] != store.Itoa(rampEnforce) {
			t.Fatalf("mode = %s, want %d", cells[0], rampEnforce)
		}
	})

	t.Run("configured to observe does not demote an enforcing ramp", func(t *testing.T) {
		db := readyDB()
		db.ramp.state = rampEnforce
		db.now = 2000
		_, cells := pkiCall(t, db, opPKIRampInit, []string{"1"})
		if cells[0] != store.Itoa(rampEnforce) {
			t.Fatalf("mode = %s, want the ramp to stay at %d", cells[0], rampEnforce)
		}
		for _, sql := range db.executed {
			if strings.Contains(sql, "ramp_state = 1") {
				t.Fatalf("init demoted the ramp: %s", sql)
			}
		}
	})

	// Not configured for mTLS at all: the store is not touched.
	t.Run("unconfigured touches nothing", func(t *testing.T) {
		db := readyDB()
		status, cells := pkiCall(t, db, opPKIRampInit, []string{"0"})
		if status != store.StatusOK || cells[0] != "0" {
			t.Fatalf("status = %d, cells = %v", status, cells)
		}
		if len(db.executed) != 0 {
			t.Fatalf("an unconfigured init ran %d statements", len(db.executed))
		}
	})
}

func TestRampGetReportsMissingBeforeInit(t *testing.T) {
	db := newPKIDB()
	db.ramp.absent = true
	status, _ := pkiCall(t, db, opPKIRampGet, nil)
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
	}

	db = readyDB()
	db.ramp.advancedAt = 4242
	status, cells := pkiCall(t, db, opPKIRampGet, nil)
	if status != store.StatusOK || len(cells) != 3 {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if cells[0] != "1" || len(cells[1]) != rosterHashLen || cells[2] != "4242" {
		t.Fatalf("cells = %v", cells)
	}
}

// --- presentation --------------------------------------------------------------

// A revoked, expired or absent serial matches no row, and reporting success
// would tell the ramp a certificate had been picked up when it had not --
// which is exactly the input the enforcement decision is made from.
func TestNotePresentationMustChangeExactlyOneRow(t *testing.T) {
	db := newPKIDB()
	status, _ := pkiCall(t, db, opPKINotePresentation, []string{"AA", "1000"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}

	db = newPKIDB()
	db.execRows = 0
	status, _ = pkiCall(t, db, opPKINotePresentation, []string{"AA", "1000"})
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}
}

// The statement must carry the guards, or presenting a revoked certificate
// would refresh it and count towards readiness.
func TestNotePresentationExcludesRevokedAndExpired(t *testing.T) {
	db := newPKIDB()
	pkiCall(t, db, opPKINotePresentation, []string{"AA", "1000"})
	sql := db.executed[0]
	if !strings.Contains(sql, "NOT revoked") {
		t.Fatalf("presentation does not exclude revoked certificates: %s", sql)
	}
	if !strings.Contains(sql, "expires_at = 0 OR expires_at > $1") {
		t.Fatalf("presentation does not exclude expired certificates: %s", sql)
	}
	if !strings.Contains(sql, "greatest(last_presented_at") {
		t.Fatalf("presentation could move the timestamp backwards: %s", sql)
	}
}

// --- lists ---------------------------------------------------------------------

func TestListsValidateBoundsAndEmitWholeRows(t *testing.T) {
	for _, fields := range [][]string{{"0"}, {"-1"}, {"99999"}, {"all"}} {
		db := newPKIDB()
		if status, _ := pkiCall(t, db, opPKICertList, fields); status != store.StatusInvalid {
			t.Fatalf("max %v: status = %d, want invalid", fields, status)
		}
		if len(db.executed) != 0 {
			t.Fatalf("an out-of-bounds list queried anyway")
		}
	}

	db := newPKIDB()
	db.certs = []cert{
		{serial: "AA", cn: "a", issued: 10, expires: 20},
		{serial: "BB", cn: "b", issued: 30, expires: 40, revoked: true},
	}
	status, cells := pkiCall(t, db, opPKICertList, []string{"10"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(cells) != 10 || len(cells)%5 != 0 {
		t.Fatalf("cells = %d, want 2 rows of 5", len(cells))
	}
	if cells[4] != "0" || cells[9] != "1" {
		t.Fatalf("revoked flags = %q %q", cells[4], cells[9])
	}
}

func TestRevokedSerialsListsOnlyRevoked(t *testing.T) {
	db := newPKIDB()
	db.certs = []cert{
		{serial: "AA", cn: "a"},
		{serial: "BB", cn: "b", revoked: true},
		{serial: "CC", cn: "c", revoked: true},
	}
	status, cells := pkiCall(t, db, opPKIRevokedSerials, []string{"10"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(cells) != 2 || cells[0] != "BB" || cells[1] != "CC" {
		t.Fatalf("cells = %v", cells)
	}
}

func TestCertUpsertValidatesItsInput(t *testing.T) {
	for _, test := range []struct {
		name   string
		fields []string
	}{
		{"empty serial", []string{"", "cn", "1", "2"}},
		{"serial too long", []string{strings.Repeat("a", pkiSerialMax+1), "cn", "1", "2"}},
		{"cn too long", []string{"AA", strings.Repeat("b", pkiCNMax+1), "1", "2"}},
		{"issued not a number", []string{"AA", "cn", "soon", "2"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newPKIDB()
			if status, _ := pkiCall(t, db, opPKICertUpsert, test.fields); status != store.StatusInvalid {
				t.Fatalf("status = %d, want invalid", status)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an invalid certificate ran %d statements", len(db.executed))
			}
		})
	}
}

// Adding or revoking a certificate changes the roster, so the stored hash must
// be re-recorded -- otherwise the next readiness check compares against a hash
// describing a fleet that has moved on.
func TestRosterChangesRefreshTheStoredHash(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"adding a certificate", opPKICertUpsert, []string{"AA", "cn", "10", "5000"}},
		{"revoking a certificate", opPKICertRevoke, []string{"AA"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newPKIDB()
			if status, _ := pkiCall(t, db, test.op, test.fields); status != store.StatusOK {
				t.Fatalf("status = %d", status)
			}
			var refreshed bool
			for _, sql := range db.executed {
				if strings.Contains(sql, "SET roster_hash = $1 WHERE singleton") {
					refreshed = true
				}
			}
			if !refreshed {
				t.Fatalf("the roster hash was not refreshed")
			}
		})
	}
}

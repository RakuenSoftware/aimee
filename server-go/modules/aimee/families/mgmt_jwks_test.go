package families

import (
	"context"
	"encoding/hex"
	"errors"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- a cache-shaped fake -----------------------------------------------------

type cachedRow struct {
	generation int64
	envelope   string
	validFrom  int64
	validUntil int64
	envDigest  []byte
	manDigest  []byte
	bundle     []byte
}

type jwksRow struct {
	row *cachedRow
	sql string
	err error
}

func (r jwksRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if r.row == nil {
		return store.ErrNoRows
	}
	switch {
	case strings.Contains(r.sql, "generation, envelope_bytes"):
		*(dest[0].(*int64)) = r.row.generation
		*(dest[1].(*string)) = r.row.envelope
		*(dest[2].(*int64)) = r.row.validFrom
		*(dest[3].(*int64)) = r.row.validUntil
		*(dest[4].(*[]byte)) = r.row.envDigest
		*(dest[5].(*[]byte)) = r.row.manDigest
		*(dest[6].(*[]byte)) = r.row.bundle
	case strings.Contains(r.sql, "SELECT generation FROM"):
		*(dest[0].(*int64)) = r.row.generation
	case strings.Contains(r.sql, "envelope_sha256, trust_bundle_sha256"):
		*(dest[0].(*[]byte)) = r.row.envDigest
		*(dest[1].(*[]byte)) = r.row.bundle
	}
	return nil
}

type jwksTx struct{ db *jwksDB }

func (t *jwksTx) Exec(_ context.Context, sql string, _ ...any) (store.Tag, error) {
	t.db.executed = append(t.db.executed, sql)
	if t.db.insertErr != nil {
		return store.RowsAffected(0), t.db.insertErr
	}
	return store.RowsAffected(1), nil
}
func (t *jwksTx) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (t *jwksTx) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	t.db.executed = append(t.db.executed, sql)
	return jwksRow{row: t.db.row, sql: sql, err: t.db.queryErr}
}
func (t *jwksTx) Commit(context.Context) error   { t.db.committed = true; return t.db.commitErr }
func (t *jwksTx) Rollback(context.Context) error { t.db.rolledBack = true; return nil }

type jwksDB struct {
	row       *cachedRow
	queryErr  error
	insertErr error
	commitErr error

	executed   []string
	committed  bool
	rolledBack bool
}

func (d *jwksDB) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(0), nil
}
func (d *jwksDB) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (d *jwksDB) QueryRow(_ context.Context, sql string, _ ...any) store.Row {
	d.executed = append(d.executed, sql)
	return jwksRow{row: d.row, sql: sql, err: d.queryErr}
}
func (d *jwksDB) Begin(context.Context) (store.Tx, error) { return &jwksTx{db: d}, nil }

func digest(b byte) []byte {
	out := make([]byte, sha256Bytes)
	for i := range out {
		out[i] = b
	}
	return out
}

func fullRow() *cachedRow {
	return &cachedRow{
		generation: 1, envelope: `{"keys":[]}`, validFrom: 100, validUntil: 200,
		envDigest: digest(0xaa), manDigest: digest(0xbb), bundle: digest(0xcc),
	}
}

// jwksCall drives the family and returns the in-band status plus the cells.
func jwksCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := MgmtJWKS.Handler(db)(
		bus.ModuleInvocation{StageID: StageMgmtJWKS}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// --- read --------------------------------------------------------------------

// A cold cache is MISSING and a broken store is FAILED. Merging them would tell
// a caller "nothing cached" about a store that never answered, and it would go
// and fetch on the strength of an absence that was never established.
func TestReadDistinguishesColdCacheFromBrokenStore(t *testing.T) {
	status, cells := jwksCall(t, &jwksDB{row: nil}, opMgmtJWKSRead, nil)
	if status != store.StatusMissing {
		t.Fatalf("cold cache status = %d, want %d (missing)", status, store.StatusMissing)
	}
	if len(cells) != 0 {
		t.Fatalf("a miss carried %d cells", len(cells))
	}

	status, _ = jwksCall(t, &jwksDB{queryErr: errors.New("store broke")}, opMgmtJWKSRead, nil)
	if status != store.StatusFailed {
		t.Fatalf("broken store status = %d, want %d (failed)", status, store.StatusFailed)
	}
}

func TestReadReturnsTheRowWithHexDigests(t *testing.T) {
	status, cells := jwksCall(t, &jwksDB{row: fullRow()}, opMgmtJWKSRead, nil)
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != 7 {
		t.Fatalf("reply carried %d cells, want 7", len(cells))
	}
	want := []string{"1", "100", "200", `{"keys":[]}`,
		hex.EncodeToString(digest(0xaa)),
		hex.EncodeToString(digest(0xbb)),
		hex.EncodeToString(digest(0xcc))}
	for i := range want {
		if cells[i] != want[i] {
			t.Fatalf("cell %d = %q, want %q", i, cells[i], want[i])
		}
	}
	// The digests must be 64 hex characters, not 32 raw bytes: this is a text
	// wire, and raw bytes would carry NULs the C side reads as terminators.
	for _, i := range []int{4, 5, 6} {
		if len(cells[i]) != 2*sha256Bytes {
			t.Fatalf("digest cell %d is %d chars, want %d", i, len(cells[i]), 2*sha256Bytes)
		}
	}
}

func TestReadRefusesACorruptRow(t *testing.T) {
	for _, test := range []struct {
		name string
		row  *cachedRow
	}{
		{"empty envelope", &cachedRow{generation: 1, envelope: "", validFrom: 1, validUntil: 2,
			envDigest: digest(1), manDigest: digest(2), bundle: digest(3)}},
		{"over-long envelope", &cachedRow{generation: 1,
			envelope:  strings.Repeat("x", jwksEnvelopeMax+1),
			validFrom: 1, validUntil: 2,
			envDigest: digest(1), manDigest: digest(2), bundle: digest(3)}},
		{"short digest", &cachedRow{generation: 1, envelope: "e", validFrom: 1, validUntil: 2,
			envDigest: []byte{1, 2}, manDigest: digest(2), bundle: digest(3)}},
	} {
		t.Run(test.name, func(t *testing.T) {
			status, _ := jwksCall(t, &jwksDB{row: test.row}, opMgmtJWKSRead, nil)
			if status != store.StatusFailed {
				t.Fatalf("status = %d, want %d -- a corrupt row is not a miss",
					status, store.StatusFailed)
			}
		})
	}
}

func TestGenerationMissingAndPresent(t *testing.T) {
	if status, _ := jwksCall(t, &jwksDB{}, opMgmtJWKSGeneration, nil); status != store.StatusMissing {
		t.Fatalf("cold generation status = %d, want missing", status)
	}
	status, cells := jwksCall(t, &jwksDB{row: fullRow()}, opMgmtJWKSGeneration, nil)
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

// --- install -----------------------------------------------------------------

func installFields() []string {
	return []string{
		"100",                                  // valid_from
		"200",                                  // valid_until
		"150",                                  // fetched_at
		hex.EncodeToString([]byte{1, 2, 3, 4}), // jwks, hex
		`{"keys":[]}`,                          // envelope
		hex.EncodeToString(digest(0xaa)),       // envelope_sha256
		hex.EncodeToString(digest(0xbb)),       // manifest_sha256
		hex.EncodeToString(digest(0xcc)),       // trust_bundle_sha256
	}
}

func TestInstallIntoAColdCacheWrites(t *testing.T) {
	db := &jwksDB{}
	status, cells := jwksCall(t, db, opMgmtJWKSInstall, installFields())
	if status != store.StatusOK || len(cells) != 1 || cells[0] != store.Itoa(jwksInstalled) {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if !db.committed {
		t.Fatalf("a fresh install did not commit")
	}
	var inserted bool
	for _, sql := range db.executed {
		if strings.Contains(sql, "INSERT") {
			inserted = true
		}
	}
	if !inserted {
		t.Fatalf("a fresh install ran no INSERT")
	}
}

// Install is install-ONCE. Re-installing the identical envelope succeeds having
// changed nothing; installing a DIFFERENT one is refused rather than silently
// replacing a signed key set with another signed key set.
func TestInstallIsOnceNotUpsert(t *testing.T) {
	t.Run("same envelope succeeds and writes nothing", func(t *testing.T) {
		db := &jwksDB{row: fullRow()}
		status, cells := jwksCall(t, db, opMgmtJWKSInstall, installFields())
		if status != store.StatusOK || cells[0] != store.Itoa(jwksInstalled) {
			t.Fatalf("status = %d, cells = %v", status, cells)
		}
		for _, sql := range db.executed {
			if strings.Contains(sql, "INSERT") {
				t.Fatalf("re-installing the same envelope wrote a row")
			}
		}
	})

	t.Run("different envelope is a conflict and writes nothing", func(t *testing.T) {
		stored := fullRow()
		stored.envDigest = digest(0x11) // not what installFields carries
		db := &jwksDB{row: stored}
		status, cells := jwksCall(t, db, opMgmtJWKSInstall, installFields())
		if status != store.StatusOK {
			t.Fatalf("status = %d, want OK with a conflict outcome", status)
		}
		if cells[0] != store.Itoa(jwksConflict) {
			t.Fatalf("outcome = %s, want %d (conflict)", cells[0], jwksConflict)
		}
		if db.committed {
			t.Fatalf("a conflicting install committed")
		}
		if !db.rolledBack {
			t.Fatalf("a conflicting install did not roll back")
		}
		for _, sql := range db.executed {
			if strings.Contains(sql, "INSERT") {
				t.Fatalf("a conflicting install wrote a row")
			}
		}
	})

	// A matching envelope digest with a DIFFERENT trust bundle is still a
	// conflict: the bundle is what the envelope is verified against, so the
	// pair has to match, not just the envelope.
	t.Run("same envelope but different trust bundle conflicts", func(t *testing.T) {
		stored := fullRow()
		stored.bundle = digest(0x22)
		db := &jwksDB{row: stored}
		_, cells := jwksCall(t, db, opMgmtJWKSInstall, installFields())
		if cells[0] != store.Itoa(jwksConflict) {
			t.Fatalf("outcome = %s, want conflict", cells[0])
		}
	})
}

func TestInstallRefusesMalformedInput(t *testing.T) {
	for _, test := range []struct {
		name  string
		index int
		value string
	}{
		{"envelope digest not hex", 5, strings.Repeat("z", 64)},
		{"envelope digest wrong length", 5, hex.EncodeToString([]byte{1, 2})},
		{"manifest digest wrong length", 6, "abcd"},
		{"bundle digest wrong length", 7, ""},
		{"jwks not hex", 3, "nothex"},
		{"jwks empty", 3, ""},
		{"envelope empty", 4, ""},
		{"valid_until before valid_from", 1, "50"},
		{"valid_from negative", 0, "-1"},
		{"fetched_at negative", 2, "-5"},
		{"valid_from not a number", 0, "soon"},
	} {
		t.Run(test.name, func(t *testing.T) {
			fields := installFields()
			fields[test.index] = test.value
			db := &jwksDB{}
			status, _ := jwksCall(t, db, opMgmtJWKSInstall, fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("a malformed install ran %d statements", len(db.executed))
			}
		})
	}
}

func TestInstallArityIsEnforced(t *testing.T) {
	frame, _ := wire.EncodeFields(opMgmtJWKSInstall, make([]string, 7))
	response, status := MgmtJWKS.Handler(&jwksDB{})(
		bus.ModuleInvocation{StageID: StageMgmtJWKS}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got, _, _ := wire.DecodeFields(response); got != store.StatusInvalid {
		t.Fatalf("in-band status = %d, want invalid", got)
	}
}

// Read and generation take no arguments, so a frame carrying some is a caller
// speaking a different contract.
func TestReadAndGenerationTakeNoArguments(t *testing.T) {
	for _, op := range []uint32{opMgmtJWKSRead, opMgmtJWKSGeneration} {
		frame, _ := wire.EncodeFields(op, []string{"unexpected"})
		response, status := MgmtJWKS.Handler(&jwksDB{row: fullRow()})(
			bus.ModuleInvocation{StageID: StageMgmtJWKS}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatalf("op %d: status = %v", op, status)
		}
		if got, _, _ := wire.DecodeFields(response); got != store.StatusInvalid {
			t.Fatalf("op %d: in-band status = %d, want invalid", op, got)
		}
	}
}

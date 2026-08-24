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

// --- a lookup-shaped fake ----------------------------------------------------

type lookupRow struct {
	value   string
	missing bool
	err     error
}

func (r lookupRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if r.missing {
		return store.ErrNoRows
	}
	*(dest[0].(*string)) = r.value
	return nil
}

type gitDB struct {
	value   string
	missing bool
	scanErr error
	execErr error

	executed []string
	args     [][]any
}

func (d *gitDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	if d.execErr != nil {
		return store.RowsAffected(0), d.execErr
	}
	return store.RowsAffected(1), nil
}
func (d *gitDB) Query(context.Context, string, ...any) (store.Rows, error) { return nil, nil }
func (d *gitDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return lookupRow{value: d.value, missing: d.missing, err: d.scanErr}
}
func (d *gitDB) Begin(context.Context) (store.Tx, error) { return gitTx{d}, nil }

type gitTx struct{ *gitDB }

func (t gitTx) Commit(context.Context) error   { return nil }
func (t gitTx) Rollback(context.Context) error { return nil }

func gitCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := GitOwnership.Handler(db)(
		bus.ModuleInvocation{StageID: StageGitOwnership}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// --- the prefix match ---------------------------------------------------------

// This is the one that mattered. The C built "<prefix>%" by concatenation and
// bound it into LIKE unescaped, so a caller-supplied prefix containing a
// wildcard matched more than it asked for -- and a bare "%" matched every
// session in the table, resolving an abbreviation to an arbitrary session.
func TestLikePrefixMatchesLiterallyNotAsAPattern(t *testing.T) {
	for _, test := range []struct{ in, want string }{
		{"abc", `abc%`},
		{"50%", `50\%%`},
		{"a_b", `a\_b%`},
		{`back\slash`, `back\\slash%`},
		{"%", `\%%`},
		{"_", `\_%`},
		{`%_\`, `\%\_\\%`},
	} {
		if got := likePrefix(test.in); got != test.want {
			t.Fatalf("likePrefix(%q) = %q, want %q", test.in, got, test.want)
		}
	}
}

// likeContains is the substring form, used by the transcript search in another
// family. The same escaping rule applies, and the wrapping is the part that
// makes it a substring match at all.
func TestLikeContainsWrapsAndEscapes(t *testing.T) {
	for _, test := range []struct{ in, want string }{
		{"deploy", `%deploy%`},
		{"50%", `%50\%%`},
		{"a_b", `%a\_b%`},
		{`back\slash`, `%back\\slash%`},
		{"%", `%\%%`},
	} {
		if got := likeContains(test.in); got != test.want {
			t.Fatalf("likeContains(%q) = %q, want %q", test.in, got, test.want)
		}
	}
	// Without the wrapping, ILIKE on a bare word is an exact comparison and a
	// search for it finds nothing.
	got := likeContains("deploy")
	if got[0] != '%' || got[len(got)-1] != '%' {
		t.Fatalf("likeContains(\"deploy\") = %q, which is not a substring pattern", got)
	}
}

// The backslash has to be escaped BEFORE the wildcards, or the escapes this
// function just introduced get escaped again.
func TestLikePrefixEscapesTheBackslashFirst(t *testing.T) {
	// If the order were wrong this would come back as `100\\%%`: the backslash
	// added for the % would itself have been doubled.
	if got := likePrefix(`100\%`); got != `100\\\%%` {
		t.Fatalf("likePrefix(`100\\%%`) = %q", got)
	}
}

func TestSessionByPrefixPassesAnEscapedPattern(t *testing.T) {
	db := &gitDB{value: "session-abc123"}
	status, cells := gitCall(t, db, opOwnershipSessionByPrefix, []string{"session-abc"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "session-abc123" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
	if len(db.args) != 1 || len(db.args[0]) != 1 {
		t.Fatalf("args = %v", db.args)
	}
	if got := db.args[0][0].(string); got != `session-abc%` {
		t.Fatalf("bound pattern %q, want an escaped prefix", got)
	}
	if !strings.Contains(db.executed[0], `ESCAPE '\'`) {
		t.Fatalf("the statement has no ESCAPE clause: %q", db.executed[0])
	}
}

// --- lookups ------------------------------------------------------------------

// A lookup with no row is MISSING; a broken store is FAILED. A caller told
// "missing" will go and claim the branch.
func TestLookupsDistinguishMissingFromFailed(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"owner get", opOwnershipOwnerGet, []string{"/repo", "main"}},
		{"branch for session", opOwnershipBranchForSession, []string{"/repo", "sess"}},
		{"session by prefix", opOwnershipSessionByPrefix, []string{"sess"}},
		{"feature branch get", opFeatureBranchGet, []string{"/repo", "sess"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			status, cells := gitCall(t, &gitDB{missing: true}, test.op, test.fields)
			if status != store.StatusMissing {
				t.Fatalf("no row: status = %d, want %d", status, store.StatusMissing)
			}
			if len(cells) != 0 {
				t.Fatalf("a miss carried %d cells", len(cells))
			}

			status, _ = gitCall(t, &gitDB{scanErr: errors.New("store broke")}, test.op, test.fields)
			if status != store.StatusFailed {
				t.Fatalf("broken postgres: status = %d, want %d", status, store.StatusFailed)
			}

			status, cells = gitCall(t, &gitDB{value: "answer"}, test.op, test.fields)
			if status != store.StatusOK || len(cells) != 1 || cells[0] != "answer" {
				t.Fatalf("status = %d, cells = %v", status, cells)
			}
		})
	}
}

// An empty stored value reads as MISSING, matching the C: its reply carried a
// fixed buffer, so "" and "no row" reached callers as the same answer.
func TestAnEmptyStoredValueIsMissing(t *testing.T) {
	status, cells := gitCall(t, &gitDB{value: ""}, opOwnershipOwnerGet, []string{"/repo", "main"})
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
	}
	if len(cells) != 0 {
		t.Fatalf("cells = %v", cells)
	}
}

// A blank repo path or branch name is a key that collides with every other
// blank one, so it is refused before it reaches the store.
func TestBlankIdentifiersAreRefusedWithoutQuerying(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"upsert blank repo", opOwnershipUpsert, []string{"", "main", "sess"}},
		{"upsert blank branch", opOwnershipUpsert, []string{"/repo", "", "sess"}},
		{"upsert blank session", opOwnershipUpsert, []string{"/repo", "main", ""}},
		{"delete blank repo", opOwnershipDelete, []string{"", "main"}},
		{"owner get blank branch", opOwnershipOwnerGet, []string{"/repo", ""}},
		{"prefix blank", opOwnershipSessionByPrefix, []string{""}},
		{"feature upsert blank", opFeatureBranchUpsert, []string{"/repo", "", "feat"}},
		{"feature get blank", opFeatureBranchGet, []string{"", "sess"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := &gitDB{value: "x"}
			status, _ := gitCall(t, db, test.op, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("a blank identifier ran %d statements", len(db.executed))
			}
		})
	}
}

// --- writes -------------------------------------------------------------------

// Both upserts replace on conflict rather than failing, so re-claiming a branch
// a session already owns is not an error.
func TestUpsertsReplaceOnConflict(t *testing.T) {
	for _, test := range []struct {
		name     string
		op       uint32
		fields   []string
		conflict string
	}{
		{"branch ownership", opOwnershipUpsert, []string{"/repo", "main", "sess"},
			"ON CONFLICT (repo_path, branch_name)"},
		{"feature branch", opFeatureBranchUpsert, []string{"/repo", "sess", "feat"},
			"ON CONFLICT (repo_path, session_id)"},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := &gitDB{}
			status, cells := gitCall(t, db, test.op, test.fields)
			if status != store.StatusOK || len(cells) != 0 {
				t.Fatalf("status = %d, cells = %v", status, cells)
			}
			if len(db.executed) != 1 {
				t.Fatalf("statements = %v", db.executed)
			}
			if !strings.Contains(db.executed[0], test.conflict) {
				t.Fatalf("statement lacks %q: %s", test.conflict, db.executed[0])
			}
			if !strings.Contains(db.executed[0], "DO UPDATE SET") {
				t.Fatalf("the upsert does not replace: %s", db.executed[0])
			}
		})
	}
}

// Deleting an unowned branch succeeds: the postcondition -- this branch is
// unowned -- holds either way.
func TestDeleteOfAnUnownedBranchSucceeds(t *testing.T) {
	db := &gitDB{}
	status, _ := gitCall(t, db, opOwnershipDelete, []string{"/repo", "gone"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
}

func TestWriteFailuresAreReported(t *testing.T) {
	db := &gitDB{execErr: errors.New("disk full")}
	status, _ := gitCall(t, db, opOwnershipUpsert, []string{"/repo", "main", "sess"})
	if status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}
}

// The single-branch reads take LIMIT 1, so without an ORDER BY they would
// return whichever row the scan reached first -- a different answer on
// successive calls with no write in between.
func TestSingleRowReadsAreOrdered(t *testing.T) {
	db := &gitDB{value: "x"}
	gitCall(t, db, opOwnershipBranchForSession, []string{"/repo", "sess"})
	gitCall(t, db, opOwnershipSessionByPrefix, []string{"sess"})
	for _, sql := range db.executed {
		if !strings.Contains(sql, "LIMIT 1") {
			continue
		}
		if !strings.Contains(sql, "ORDER BY") {
			t.Fatalf("a LIMIT 1 read has no ORDER BY: %s", sql)
		}
	}
}

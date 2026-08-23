package postgres

import (
	"context"
	"testing"
)

// Numbers, pinned as numbers.
//
// These exist because the two halves of this wire DISAGREED and neither
// noticed: the served side had LimitExceeded at 2, the calling side had it at
// 3, and both were written confidently against their own constant. A size
// refusal carries no SQLSTATE, so the mismatch would have reached the caller as
// an unexplained failure, been retried, refused identically, and never
// explained.
//
// A name proves nothing here. Renaming a constant changes no bytes; changing a
// number changes what the far end believes happened.

func TestStoreStatusIntegersArePinned(t *testing.T) {
	for name, got := range map[string]struct{ have, want uint32 }{
		"OK":              {statusOK, 0},
		"InvalidRequest":  {statusInvalidRequest, 1},
		"Unsupported":     {statusUnsupported, 2},
		"LimitExceeded":   {statusLimitExceeded, 3},
		"StatementFailed": {statusStatementFailed, 4},
		"Unavailable":     {statusUnavailable, 5},
		"MigrationFailed": {statusMigrationFailed, 6},
	} {
		if got.have != got.want {
			t.Errorf("%s = %d, want %d; the far end reads the number, not the name",
				name, got.have, got.want)
		}
	}
}

func TestOperationCodesArePinned(t *testing.T) {
	for name, got := range map[string]struct{ have, want uint32 }{
		"Exec":           {opExec, 1},
		"Query":          {opQuery, 2},
		"Begin":          {opBegin, 3},
		"Commit":         {opCommit, 4},
		"Rollback":       {opRollback, 5},
		"Migrate":        {opMigrate, 6},
		"CurrentVersion": {opCurrentVersion, 7},
	} {
		if got.have != got.want {
			t.Errorf("%s = %d, want %d", name, got.have, got.want)
		}
	}
}

func TestValueTypeNumbersArePinned(t *testing.T) {
	// Appended, never renumbered. Inserting a type reinterprets every value
	// already encoded against the old numbering, silently and both ways.
	for name, got := range map[string]struct{ have, want uint8 }{
		"null":  {typeNull, 0},
		"text":  {typeText, 1},
		"int":   {typeInt, 2},
		"float": {typeFloat, 3},
		"bool":  {typeBool, 4},
		"texts": {typeTexts, 5},
		"bytes": {typeBytes, 6},
	} {
		if got.have != got.want {
			t.Errorf("%s = %d, want %d", name, got.have, got.want)
		}
	}
}

// Known-answer vectors for the migration checksum.
//
// Both sides compute this independently -- the caller to send it, the module to
// verify it against the statements it actually received -- and what it detects
// is a disagreement about what ran. So a change to the construction on one side
// alone turns every recorded migration into an apparent edit and refuses a
// database that is in fact correct.
//
// Fixed digests rather than a property, because a test that recomputes the
// expectation the same way as the code agrees with any change to both.
func TestMigrationChecksumVectors(t *testing.T) {
	for name, test := range map[string]struct {
		statements []string
		want       string
	}{
		"no statements": {nil,
			"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		"one statement": {[]string{"SELECT 1"},
			"49f49ad546a92d979e21613fd504e7cf66dd6c095299aa2703f3648ff38ab9cb"},
		"two statements": {[]string{"a", "b"},
			"b9a9ea4253503b08401214780e55a8aaf3cdfa55697ba34ed65a55634d86c423"},
		"one joined statement": {[]string{"a;b"},
			"1f6782a6c1fd43b1a5824aaa51104e42fbb87fff55ffd50c6bf9bf12d17c2279"},
	} {
		if got := Checksum(test.statements); got != test.want {
			t.Errorf("%s: checksum = %s, want %s", name, got, test.want)
		}
	}

	// The property those vectors encode: boundaries are part of the digest, so
	// an edit that merges two statements into one cannot read as no change.
	if Checksum([]string{"a;b"}) == Checksum([]string{"a", "b"}) {
		t.Error("statement boundaries do not affect the checksum")
	}
	if Checksum([]string{"a", "b"}) == Checksum([]string{"b", "a"}) {
		t.Error("statement order does not affect the checksum")
	}
	if Checksum([]string{""}) == Checksum(nil) {
		t.Error("an empty statement hashes the same as no statements")
	}
}

// The refusals a caller must be able to tell apart.
//
// Each means something different and wants a different response, so a test that
// only checked "an error came back" would pass while the caller acted on the
// wrong one.
func TestEachRefusalIsDistinguishable(t *testing.T) {
	for _, test := range []struct {
		name      string
		status    uint32
		sqlstate  string
		notClosed bool
		notUnique bool
	}{
		{"result too large", statusLimitExceeded, "54000", true, true},
		{"transaction cap", statusLimitExceeded, "53300", true, true},
		{"database unreachable", statusUnavailable, "", true, true},
		{"unique violation", statusStatementFailed, "23505", true, false},
		{"transaction gone", statusStatementFailed, "25P01", false, true},
	} {
		client := New(func(context.Context, []byte) ([]byte, error) {
			return failReply(test.status, test.sqlstate, test.name), nil
		})
		_, err := client.Exec(context.Background(), "op", 0, "SELECT 1")
		if err == nil {
			t.Fatalf("%s: no error", test.name)
		}
		if test.notClosed && IsTransactionClosed(err) {
			t.Errorf("%s read as a closed transaction; a caller would abandon "+
				"work that is still live", test.name)
		}
		if test.notUnique && IsUniqueViolation(err) {
			t.Errorf("%s read as a unique violation; a caller would treat a "+
				"refusal as an expected duplicate", test.name)
		}
		// An oversized result must never arrive as an empty one. That variant
		// is the worst of them: a caller handed no rows stops asking, so
		// nothing ever corrects it.
		if test.status == statusLimitExceeded {
			rows, queryErr := client.Query(context.Background(), "op", 0, "SELECT 1")
			if queryErr == nil {
				t.Errorf("%s: an over-ceiling result returned %d rows instead of "+
					"refusing", test.name, len(rows))
			}
		}
	}
}

// An unreachable store is not a refusal. A caller that reads "the database is
// down" as "the database said no" records an outage as a decision.
func TestUnavailableIsNotAConstraintFailure(t *testing.T) {
	client := New(func(context.Context, []byte) ([]byte, error) {
		return failReply(statusUnavailable, "", "database unavailable"), nil
	})
	_, err := client.Exec(context.Background(), "op", 0, "SELECT 1")
	if err == nil {
		t.Fatal("an unavailable database reported success")
	}
	if IsUniqueViolation(err) || IsCheckViolation(err) ||
		IsForeignKeyViolation(err) || IsNotNullViolation(err) {
		t.Error("an outage classified as a constraint failure")
	}
}

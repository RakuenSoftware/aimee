package db

import (
	"errors"
	"testing"
)

// The parts of the contract that were agreed after the first implementation.

func TestAnOverLargeResultIsRefusedNotTruncated(t *testing.T) {
	// The store answers StoreStatusLimitExceeded rather than framing a capped set: a caller
	// handed the ceiling could not tell it from a complete answer, and would
	// record the cap as fact. That is NULL-versus-empty in another costume.
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(StoreStatusLimitExceeded).str("").
		str("42000 rows exceeds the ceiling").b}
	db := newStore(t, f)

	_, err := db.Query(t.Context(), "SELECT * FROM big")
	if !errors.Is(err, ErrResultTooLarge) {
		t.Fatalf("over-large result = %v, want ErrResultTooLarge", err)
	}
	// And it is not mistaken for an empty answer or for an outage: one of those
	// would be recorded as fact and the other retried.
	if errors.Is(err, ErrNoRows) || errors.Is(err, ErrStoreUnavailable) {
		t.Error("a refused result was classified as empty or unreachable")
	}
}

func TestAnOverLargeResultIsNotAConstraintRefusal(t *testing.T) {
	// StoreStatusLimitExceeded carries no SQLSTATE, so it must not fall through to the
	// classifier and read as a violation of something.
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(StoreStatusLimitExceeded).str("").str("too many rows").b}
	db := newStore(t, f)
	_, err := db.Query(t.Context(), "SELECT * FROM big")
	if IsUniqueViolation(err) || IsForeignKeyViolation(err) ||
		IsCheckViolation(err) || IsNotNullViolation(err) {
		t.Errorf("a size refusal was classified as a constraint violation: %v", err)
	}
}

// TestTheChecksumConstructionIsPinned is the known-answer test both sides can
// run.
//
// The store recomputes the checksum over the statements it actually received
// rather than trusting the one sent with them -- otherwise the tamper detection
// is defeated by exactly what it exists to catch. That only works if the two
// implementations hash identically, and "identically" is not something either
// side can verify by reading the other's prose.
//
// So the construction is pinned to fixed vectors: len(statement) NUL statement
// NUL, per statement, in order. If the postgres module produces these digests
// for these inputs, the two agree.
func TestTheChecksumConstructionIsPinned(t *testing.T) {
	for _, tc := range []struct {
		name       string
		statements []string
		want       string
	}{
		{
			name:       "one statement",
			statements: []string{"CREATE TABLE t (a int);"},
			want:       checksumVectorOne,
		},
		{
			name:       "two statements",
			statements: []string{"A", "B"},
			want:       checksumVectorTwo,
		},
		{
			name: "a split cannot collide with a join",
			// "A" + "B" concatenated is "AB"; length prefixing is what keeps
			// these apart, and a separator alone would not.
			statements: []string{"AB"},
			want:       checksumVectorJoined,
		},
		{
			name:       "no statements",
			statements: nil,
			want:       checksumVectorEmpty,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := StoreChecksum(tc.statements); got != tc.want {
				t.Errorf("checksum = %s, want %s", got, tc.want)
			}
		})
	}

	// The property the vectors exist to protect.
	if StoreChecksum([]string{"AB"}) == StoreChecksum([]string{"A", "B"}) {
		t.Error("a joined statement hashes the same as the split one")
	}
}

// TestTheStoreStatusValuesArePinned is the other known-answer test.
//
// The checksum vectors let both sides confirm they hash identically. These do
// the same for the reply header. A status is a bare integer on the wire, so
// agreeing on the NAME "limit exceeded" agrees on nothing: two implementations
// can each be internally consistent, pass all their own tests, and still
// disagree about which number carries it.
//
// The failure that would cause is the quiet kind. A mismatch on the size
// refusal does not fail loudly -- the reply carries no SQLSTATE, so it falls
// through to the classifier and is recorded as an unexplained failure rather
// than a result too large to send. The caller retries, gets the same answer,
// and nothing ever says why.
func TestTheStoreStatusValuesArePinned(t *testing.T) {
	for _, tc := range []struct {
		name string
		got  uint32
		want uint32
	}{
		{"ok", StoreStatusOK, 0},
		{"limit exceeded", StoreStatusLimitExceeded, 3},
		{"failed", StoreStatusFailed, 4},
	} {
		if tc.got != tc.want {
			t.Errorf("%s = %d, want %d -- changing this is a wire break",
				tc.name, tc.got, tc.want)
		}
	}

	// And the mapping that rests on them, end to end: a literal 3 on the wire
	// must reach the caller as ErrResultTooLarge, not as a generic refusal.
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(3).str("").str("42 rows over the ceiling").b}
	_, err := newStore(t, f).Query(t.Context(), "SELECT * FROM big")
	if !errors.Is(err, ErrResultTooLarge) {
		t.Errorf("status 3 = %v, want ErrResultTooLarge", err)
	}
}

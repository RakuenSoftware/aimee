package result

import (
	"errors"
	"fmt"
	"testing"
)

func TestErrIsNilForAnythingThatIsNotAFailure(t *testing.T) {
	// Pending is the one worth pinning: work that has not finished has not
	// failed, and a caller that treats "still running" as an error abandons
	// something that was going to succeed.
	for _, code := range []Code{Pending, Done, DoneNoChange, DoneAlready, DonePartial,
		DoneEmpty, DoneRefused} {
		if err := Err(code); err != nil {
			t.Errorf("Err(%s) = %v, want nil", code, err)
		}
	}
}

func TestErrCarriesTheFailure(t *testing.T) {
	for _, code := range []Code{Failed, Invalid, Denied, Unauthenticated, Unavailable,
		Conflict, NotFound, TooLarge, Timeout, Integrity, Cancelled} {
		err := Err(code)
		if err == nil {
			t.Fatalf("Err(%d) = nil, want an error", code)
		}
		if got := CodeOf(err); got != code {
			t.Errorf("CodeOf(Err(%d)) = %d", code, got)
		}
	}
}

func TestCodeSurvivesWrapping(t *testing.T) {
	wrapped := fmt.Errorf("reading the grant: %w", Err(Denied))
	if got := CodeOf(wrapped); got != Denied {
		t.Errorf("CodeOf(wrapped) = %s, want denied", got)
	}
	if !errors.Is(wrapped, Err(Denied)) {
		t.Error("errors.Is did not match on the code through a wrap")
	}
	if errors.Is(wrapped, Err(NotFound)) {
		t.Error("errors.Is matched a different code")
	}
}

func TestNilErrorIsDoneNotPending(t *testing.T) {
	// A Go function that has returned has finished, whatever it was doing.
	// Pending reaches Go from a job row or a boundary, never from a return.
	if got := CodeOf(nil); got != Done {
		t.Errorf("CodeOf(nil) = %s, want done", got)
	}
}

func TestUncodedErrorIsFailed(t *testing.T) {
	// The honest answer: something went wrong and this layer does not know
	// which thing. Anything else would be inventing information.
	if got := CodeOf(errors.New("no code here")); got != Failed {
		t.Errorf("CodeOf(plain) = %s, want failed", got)
	}
}

func TestStringNamesWhatItKnowsAndNothingElse(t *testing.T) {
	cases := map[Code]string{
		Pending:      "pending",
		DoneNoChange: "done, no change",
		Denied:       "denied",
		Code(42):     "result 42",
	}
	for code, want := range cases {
		if got := code.String(); got != want {
			t.Errorf("Code(%d).String() = %q, want %q", code, got, want)
		}
	}
}

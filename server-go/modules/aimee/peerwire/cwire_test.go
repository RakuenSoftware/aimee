package peerwire

import (
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// cImplText is the C client's implementation, where the parsing lives. The
// header carries the contract; this carries whether it is honoured.
const cImpl = "../../../../src/peer_client/peer_client.c"

func cImplText(t *testing.T) string {
	t.Helper()
	b, err := os.ReadFile(cImpl)
	if err != nil {
		t.Fatalf("cannot read the C client at %s: %v", cImpl, err)
	}
	return string(b)
}

// The C client in src/peer_client transcribes this package's status numbers and
// row width, because those integers cross a process boundary and C cannot import
// Go. A transcription nobody checks is a copy free to drift, and drift here is
// silent in the worst way: every refusal keeps arriving, each one named as a
// different refusal than the module sent. A renumber would turn `denied` into
// `inbox_full` with no error anywhere.
//
// So this reads the header and compares. It is deliberately on the GO side: the
// C test cannot see this file, and asserting the match from a place that can see
// only one of the two is how a pin ends up pinning a value to itself.
const cHeader = "../../../../src/peer_client/peer_client.h"

func cHeaderText(t *testing.T) string {
	t.Helper()
	b, err := os.ReadFile(cHeader)
	if err != nil {
		// NOT skipped. A missing header means the client was moved or deleted,
		// and skipping would report success for a check that did not run --
		// which is precisely the "evidence mistaken for a check" shape this
		// package's tests exist to refuse.
		t.Fatalf("cannot read the C client header at %s: %v", cHeader, err)
	}
	return string(b)
}

func TestCClientPinsTheSameStatusNumbers(t *testing.T) {
	src := cHeaderText(t)
	re := regexp.MustCompile(`PEER_CLIENT_STATUS_([A-Z_]+) = (\d+)`)
	found := map[string]int{}
	for _, m := range re.FindAllStringSubmatch(src, -1) {
		n, err := strconv.Atoi(m[2])
		if err != nil {
			t.Fatalf("status %s has an unparsable value %q", m[1], m[2])
		}
		found[m[1]] = n
	}
	// Names as the C header spells them, in this package's numeric order. The
	// list is checked for COMPLETENESS below against StatusCount, so adding a
	// status to either side without the other fails here rather than shipping.
	want := []string{
		"OK", "NO_PEER", "DENIED", "INBOX_FULL", "HOP_LIMIT", "CYCLE", "TIMEOUT",
		"SELF", "TOO_LONG", "LABEL_TAKEN", "UNKNOWN_SENDER", "BAD_REQUEST",
		"SHUTDOWN", "NO_CHANNEL", "NOT_MEMBER", "CHANNEL_FULL", "UNAVAILABLE",
		"UNCLASSIFIED", "AT_CAPACITY", "NO_DIRECTORY", "DIRECTORY_REFUSED",
	}
	if len(want) != StatusCount {
		t.Fatalf("this test lists %d statuses but StatusCount is %d: a status was "+
			"added to peerwire and not to this list", len(want), StatusCount)
	}
	for i, name := range want {
		got, ok := found[name]
		if !ok {
			t.Errorf("the C header has no PEER_CLIENT_STATUS_%s, so a %s refusal "+
				"reaches C unnamed", name, Status(i))
			continue
		}
		if got != i {
			t.Errorf("PEER_CLIENT_STATUS_%s is %d in C and %d in Go: every refusal "+
				"at or after this one is misread", name, got, i)
		}
	}
	// COUNT is what tells the C side that a status it does not know is unknown
	// rather than one it does. If it lags, an added status reads as a named one.
	if got := found["COUNT"]; got != StatusCount {
		t.Errorf("PEER_CLIENT_STATUS_COUNT is %d in C and StatusCount is %d",
			got, StatusCount)
	}
	// A status in C that is NOT in the list above is a status Go does not have.
	// Checked because the two loops above only walk Go's names: without this, C
	// could carry an extra number and every guard here would still pass.
	for name := range found {
		if name == "COUNT" {
			continue
		}
		known := false
		for _, w := range want {
			if w == name {
				known = true
				break
			}
		}
		if !known {
			t.Errorf("the C header declares PEER_CLIENT_STATUS_%s, which peerwire "+
				"does not have", name)
		}
	}
}

func TestCClientPinsTheMessageWidth(t *testing.T) {
	src := cHeaderText(t)
	m := regexp.MustCompile(`define PEER_CLIENT_MESSAGE_WIDTH (\d+)`).FindStringSubmatch(src)
	if m == nil {
		t.Fatal("the C header does not define PEER_CLIENT_MESSAGE_WIDTH")
	}
	got, err := strconv.Atoi(m[1])
	if err != nil {
		t.Fatalf("unparsable width %q", m[1])
	}
	// Cells APPEND, so a width mismatch is not a cosmetic disagreement: the C
	// client divides a list reply by this number and refuses a remainder. Off by
	// one, every take of more than one message is refused as malformed -- or
	// worse, divides evenly and hands back rows made of two halves.
	if got != MessageWidth {
		t.Errorf("PEER_CLIENT_MESSAGE_WIDTH is %d and peerwire.MessageWidth is %d",
			got, MessageWidth)
	}
}

// TestCClientSpeaksTheSameBooleanGrammar is the guard that was missing.
//
// This file pinned the status numbers and the row width and stopped there, so
// the two sides agreed about how many cells a row has and disagreed about what
// is IN one. Btoa writes "1"/"0"; the C reader accepted only "true"/"false",
// which Btoa NEVER writes -- so it rejected every message row the module has
// ever sent, and both test suites stayed green because each had been written
// from its own side's assumption. The C fixture even spelled the cell "false",
// confirming the misreading it was supposed to catch.
//
// Atob's leniency is what let it survive undetected: it accepts "true"/"false"
// as well, so the C client's REQUESTS were understood and only the reply
// direction broke. Half a conversation working is worse than none, because at
// the caller it reads as the far side failing rather than as a grammar this
// side got wrong.
func TestCClientSpeaksTheSameBooleanGrammar(t *testing.T) {
	body := cImplText(t)

	// Whatever Btoa can WRITE, the C side must be able to READ. Asserted against
	// Btoa's real output rather than a hand-copied list, so a change to Btoa
	// fails here instead of quietly leaving the C reader behind.
	for _, v := range []bool{true, false} {
		lit := `"` + Btoa(v) + `"`
		if !strings.Contains(body, "strcmp(cells[8], "+lit+")") {
			t.Errorf("peerwire.Btoa(%v) writes %s and the C client never compares against it, "+
				"so every row carrying that value is rejected as malformed", v, lit)
		}
	}

	// And whatever the C side WRITES for a bool, Atob must accept -- otherwise
	// the module refuses the request and the caller is told its own well-formed
	// call was wrong.
	if !strings.Contains(body, `expect_reply ? "1" : "0"`) {
		t.Error(`the C client no longer writes expect_reply as "1"/"0"; if the spelling ` +
			`changed, check peerwire.Atob still accepts what it now writes`)
	}
	for _, spelled := range []string{"1", "0"} {
		if _, err := Atob(spelled); err != nil {
			t.Errorf("Atob rejects %q, which the C client writes for a bool: %v", spelled, err)
		}
	}
}

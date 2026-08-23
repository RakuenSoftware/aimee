package main

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The validation record is a hand-transcribed copy of what the probe printed on
// a container that no longer exists. That makes it the one artefact here with no
// build to fail: the containers were destroyed under the cleanup rule, so the
// run cannot be repeated to check the table, and a row dropped in transcription
// stays dropped.
//
// It had already drifted. The probe made fifteen checks and the record claimed
// fourteen, listing fourteen rows -- "take on an unknown session is refused" was
// simply never copied across. Both landed in the SAME commit, so this was not a
// doc left behind by later work; it was wrong the day it was written.
//
// So the record is asserted against the probe's source. This cannot re-run the
// container and does not pretend to: it checks that the record DESCRIBES the
// probe it claims to describe. A check added here without a row is a run whose
// evidence is incomplete, which is the direction that matters -- a record that
// under-claims is how a passing check goes missing.
var validationRecord = filepath.Join("..", "..", "..", "..", "..",
	"docs", "validation", "aimee-module-on-a-clean-container.md")

func TestValidationRecordMatchesTheProbe(t *testing.T) {
	source, err := os.ReadFile("main.go")
	if err != nil {
		t.Fatalf("read probe: %v", err)
	}
	record, err := os.ReadFile(validationRecord)
	if err != nil {
		t.Fatalf("read validation record: %v", err)
	}

	// Plain check names, which the record can be matched against by text.
	names := regexp.MustCompile(`check\("([^"]*)"`).FindAllStringSubmatch(string(source), -1)
	// Names built with fmt.Sprintf carry a kind number, so they are counted but
	// not matched: the number is a property of the run, not of the source.
	formatted := strings.Count(string(source), "check(fmt.Sprintf(")
	rows := strings.Count(string(record), "| pass |")

	if got := len(names) + formatted; got != rows {
		t.Errorf("probe makes %d checks; the record lists %d rows. A check with no "+
			"row is a passing result missing from the evidence.", got, rows)
	}

	// Matched against THE ROWS ONLY, not the whole document.
	//
	// This used to search the entire record, and the record quotes probe output
	// verbatim in its prose. So a deleted row could be satisfied by the
	// narrative ABOUT that row -- "send between two directory-known sessions"
	// appears at line 115 as quoted output and at line 265 as the row, and
	// searching the document finds it either way.
	//
	// A peer hit the same thing an hour ago from the other side: their scan
	// counted a file as coupled to the store because it matched `db1_conn` in a
	// comment that said the file does not link the store. Prose about a thing
	// and the thing itself are not distinguishable to a text search, and the
	// prose is usually the more quotable of the two.
	var tableLines strings.Builder
	for _, line := range strings.Split(string(record), "\n") {
		if trimmed := strings.TrimSpace(line); strings.HasPrefix(trimmed, "|") {
			tableLines.WriteString(strings.ToLower(line))
			tableLines.WriteString("\n")
		}
	}
	table := tableLines.String()
	for _, n := range names {
		// Match on a distinctive prefix: the record reworded some rows for
		// prose, and pinning whole strings would fail on wording rather than on
		// coverage.
		head := n[1]
		if len(head) > 24 {
			head = head[:24]
		}
		if !strings.Contains(table, strings.ToLower(head)) {
			t.Errorf("probe check %q has no ROW in the validation record. Prose "+
				"quoting the check does not count: the table is the evidence.", n[1])
		}
	}
}

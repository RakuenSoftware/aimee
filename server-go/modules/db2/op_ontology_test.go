package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestNormalizeRelType(t *testing.T) {
	// Every ontology statement binds the normalized form, so this function is
	// what decides whether two spellings are the same relation. Getting it
	// wrong splits an ontology in two without anything failing.
	for _, testCase := range []struct{ in, want, why string }{
		{"works_for", "works_for", "already normal"},
		{"worksFor", "works_for", "camelCase boundary starts a word"},
		{"WorksFor", "works_for", "a leading capital is not a boundary"},
		{"WORKS_FOR", "works_for", "runs of capitals are not boundaries"},
		{"works for", "works_for", "punctuation becomes an underscore"},
		{"works---for", "works_for", "a run of punctuation collapses to one"},
		{"__works_for__", "works_for", "leading suppressed, trailing stripped"},
		{"  works for  ", "works_for", "leading and trailing space likewise"},
		{"works2for", "works2for", "digits are word characters"},
		{"works2For", "works2_for", "a capital after a digit is a boundary"},
		{"", "", "nothing normalizes to nothing"},
		{"---", "", "punctuation only is not a relation"},
		{"_", "", "an underscore alone is not a relation"},
	} {
		if got := normalizeRelType(testCase.in); got != testCase.want {
			t.Errorf("normalizeRelType(%q) = %q, want %q -- %s",
				testCase.in, got, testCase.want, testCase.why)
		}
	}
}

func TestNormalizeRelTypeTruncatesRatherThanRefuses(t *testing.T) {
	// A buffer size in the C, which makes it a silent truncation point: two
	// names agreeing in their first 63 characters are one relation. Pinned
	// because it is surprising and because changing it would silently merge or
	// split relations in an existing ontology.
	long := strings.Repeat("a", relTypeNameMax+20)
	got := normalizeRelType(long)
	if len(got) != relTypeNameMax {
		t.Fatalf("length = %d, want %d", len(got), relTypeNameMax)
	}
	if normalizeRelType(long) != normalizeRelType(long+"different") {
		t.Error("two names agreeing in their first 63 characters normalized apart")
	}
}

func TestNormalizeRelTypeWalksBytesNotRunes(t *testing.T) {
	// The C walks with isalnum on unsigned char, so every byte of a multi-byte
	// character is non-alphanumeric and the whole character collapses to one
	// underscore. Decoding UTF-8 here would normalize differently and split the
	// ontology between the C and the port.
	// The two bytes of U+00E9, written as bytes rather than as the character,
	// because that is what the function sees and because the source file is
	// swept for non-ASCII to catch smart quotes arriving by accident.
	if got := normalizeRelType("works\xc3\xa9for"); got != "works_for" {
		t.Errorf("normalizeRelType with a multi-byte character = %q, want %q",
			got, "works_for")
	}
}

func TestOntologyDecisionsMoveBothTablesTogether(t *testing.T) {
	// A relation marked active in rel_types with no approved evaluation behind
	// it is a permission granted by nobody.
	for _, testCase := range []struct {
		name     string
		stage    uint32
		build    func() ([]byte, error)
		decision string
		relTypes string
	}{
		{
			"approve",
			db2contract.StageOntologyApprove,
			func() ([]byte, error) { return db2contract.EncodeOntologyApproveRequest("worksFor") },
			"'approved'",
			"'active'",
		},
		{
			"reject",
			db2contract.StageOntologyReject,
			func() ([]byte, error) { return db2contract.EncodeOntologyRejectRequest("worksFor") },
			"'rejected'",
			"'rejected'",
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if store.txCalls != 1 || !store.committed {
				t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
			}
			if len(store.sqlLog) != 2 {
				t.Fatalf("statements = %d, want the evaluation and the rel_type",
					len(store.sqlLog))
			}
			if !strings.Contains(store.sqlLog[0], "ontology_evaluations") ||
				!strings.Contains(store.sqlLog[0], testCase.decision) {
				t.Errorf("the evaluation was not decided: %q", store.sqlLog[0])
			}
			if !strings.Contains(store.sqlLog[1], "rel_types") ||
				!strings.Contains(store.sqlLog[1], testCase.relTypes) {
				t.Errorf("the relation type was not moved: %q", store.sqlLog[1])
			}
			// Normalized before binding, on both statements.
			for index, args := range store.argsLog {
				if len(args) != 1 || args[0] != "works_for" {
					t.Fatalf("statement %d bound %v, want the normalized name",
						index, args)
				}
			}
		})
	}
}

func TestOntologyDecisionNeedsAnOpenEvaluation(t *testing.T) {
	// Deciding on a relation nobody proposed is the caller getting it wrong,
	// and the rel_types change must not stand on its own.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyApproveRequest("never_proposed")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyApprove), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyApproveReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %d -- rel_types moved without an evaluation",
			len(store.sqlLog))
	}
}

func TestOntologyDecisionSurvivesRelTypesMatchingNothing(t *testing.T) {
	// A seeded relation type may already carry the status the decision would
	// set. Refusing a correctly recorded decision over that would be wrong.
	store := &fakeStore{}
	store.execRowsAt = false // the evaluation matches one row
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyApproveRequest("already_active")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyApprove), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyApproveReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !store.committed {
		t.Error("the decision was not committed")
	}
}

func TestOntologyRefusesANameThatNormalizesToNothing(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyApproveRequest("---")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyApprove), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeOntologyApproveReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.txCalls != 0 {
		t.Error("a transaction was opened for a name that is not a relation")
	}
}

func TestEvalStatusNormalizesBeforeReading(t *testing.T) {
	// The stored form is normalized, so a caller asking with the spelling they
	// have must still find it.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("approved")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeOntologyEvalStatusRequest("WorksFor")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageOntologyEvalStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	state, decodeErr := db2contract.DecodeOntologyEvalStatusReply(body)
	if decodeErr != nil || state != "approved" {
		t.Fatalf("status = %q", state)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != "works_for" {
		t.Fatalf("args = %v, want the normalized name", store.lastArgs)
	}
}

func TestReleaseCreateAnswersZeroForADuplicateName(t *testing.T) {
	// The name is unique, so creating one twice is a constraint violation
	// rather than a second release -- which is what stops a retry from forking
	// the release history. Zero says no release was created.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeReleaseCreateRequest("already-taken")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageReleaseCreate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeReleaseCreateReply(body)
	if decodeErr != nil || id != 0 {
		t.Fatalf("id = %d, want 0", id)
	}
}

func TestReleaseCreateLeavesEveryOtherColumnDefaulted(t *testing.T) {
	// A fresh release is 'pending' with no promoted or retired timestamp;
	// kb_release_promote is what moves it. Naming a state here would create one
	// that is already active without the pointer that makes it so.
	store := &fakeStore{row: &fakeRow{values: []any{int64(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeReleaseCreateRequest("live-probe")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageReleaseCreate), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "state") ||
		strings.Contains(store.lastSQL, "promoted_at") {
		t.Errorf("the insert names a column it should leave defaulted: %q", store.lastSQL)
	}
}

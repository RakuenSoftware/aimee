package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEvidenceStoreVectorReplacesAndMarksTogether(t *testing.T) {
	// The C runs the three statements unwrapped. The window that matters is the
	// delete succeeding and the insert failing: the artifact then has no vector,
	// and if its queue row already said 'ok' nothing retries it -- a silent hole
	// rather than a visible failure.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEvidenceStoreVectorRequest(
		"artifact-1", "evidence", "[0.1,0.2]")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEvidenceStoreVector), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeEvidenceStoreVectorReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) != 3 ||
		!strings.Contains(store.sqlLog[0], "DELETE FROM evidence_vectors") ||
		!strings.Contains(store.sqlLog[1], "INSERT INTO evidence_vectors") ||
		!strings.Contains(store.sqlLog[2], "evidence_index_ops") {
		t.Fatalf("statements = %v", store.sqlLog)
	}
}

func TestEvidenceStoreVectorRollsBackAPartialReplace(t *testing.T) {
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEvidenceStoreVectorRequest(
		"artifact-1", "evidence", "[0.1]")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEvidenceStoreVector), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeEvidenceStoreVectorReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
}

func TestEvidenceStoreVectorSubstitutesEmptyFields(t *testing.T) {
	// A vector row with no collection is not found by the reads that filter on
	// one, and an empty string is not a valid vector literal. The encoder
	// permits both, so the substitution has to happen here.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEvidenceStoreVectorRequest("artifact-1", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEvidenceStoreVector), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	insert := store.argsLog[1]
	if len(insert) != 3 || insert[1] != "evidence" || insert[2] != "[]" {
		t.Fatalf("insert args = %v, want the substituted defaults", insert)
	}
}

func TestMiningHighWaterMarkOnlyMovesForward(t *testing.T) {
	// A pass that failed partway reports how far it reached, which can be
	// behind where a previous pass finished. Taking the lower number would make
	// the next pass re-mine work already done, and repeating that is how a job
	// stops making progress.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMiningJobCompleteRequest("job-1", 400, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMiningJobComplete), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "hwm = CASE WHEN hwm > $2 THEN hwm ELSE $2 END") {
		t.Errorf("the high-water mark can now move backwards: %q", store.lastSQL)
	}
	// The error is written unconditionally, so a successful pass clears the
	// previous failure rather than leaving it attached.
	if !strings.Contains(store.lastSQL, "last_error = $3") ||
		strings.Contains(store.lastSQL, "last_error = CASE") {
		t.Errorf("a stale error would survive a successful pass: %q", store.lastSQL)
	}
}

func TestKbDirectiveResolveGuardsItselfInTheStatement(t *testing.T) {
	// The C reads the state first and then updates without the predicate, so
	// between its check and its write another caller can resolve the directive
	// and the second write overwrites which memory answered it. Folding the
	// check into the WHERE closes that, and matches the sibling operation.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDirectiveResolveRequest(4, 9, "because")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDirectiveResolve), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBDirectiveResolveReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d for a directive that was not open", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "WHERE id = $1 AND state = 'open'") {
		t.Errorf("the open check left the statement: %q", store.lastSQL)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d -- the check and the write can be interleaved",
			store.execCalls)
	}
	// The note is decoded and dropped: there is no column for it, and the C
	// casts the parameter to void on its first line.
	if len(store.lastArgs) != 2 {
		t.Fatalf("args = %v -- the note reached a statement with nowhere to put it",
			store.lastArgs)
	}
}

func TestResolveContradictionMatchesEitherOrdering(t *testing.T) {
	// A contradiction between A and B is the same one as between B and A, so
	// matching a single direction would leave half of them unresolvable.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeResolveContradictionRequest(4, 9, 12)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageResolveContradiction), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "(memory_a_id = $1 AND memory_b_id = $2)") ||
		!strings.Contains(store.lastSQL, "(memory_a_id = $2 AND memory_b_id = $1)") {
		t.Errorf("only one ordering is matched: %q", store.lastSQL)
	}
	// Restricted to open contradictions, so resolving a pair cannot silently
	// close some other question naming the same two memories.
	if !strings.Contains(store.lastSQL, "state = 'open' AND cause = 'contradiction'") {
		t.Errorf("the resolve is no longer scoped to open contradictions: %q",
			store.lastSQL)
	}
}

func TestConflictingL2NeedsConfidenceAndDifferentContent(t *testing.T) {
	// Three conditions make it a conflict rather than a duplicate: the same
	// key, different content, and enough confidence to be worth arguing with.
	// The floor is what stops every half-believed memory raising a
	// contradiction.
	store := &fakeStore{row: &fakeRow{values: []any{ptrFloat(0.9)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryConflictingL2Request("deploy:target", "staging")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryConflictingL2), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, confidence, decodeErr := db2contract.DecodeMemoryConflictingL2Reply(body)
	if decodeErr != nil || found != 1 || confidence != 0.9 {
		t.Fatalf("found = %d, confidence = %v", found, confidence)
	}
	if !strings.Contains(store.lastSQL, "confidence >= 0.8") {
		t.Errorf("the confidence floor is gone: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "content != $2") {
		t.Errorf("a duplicate would now read as a conflict: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "tier = 'L2'") {
		t.Errorf("the tier restriction is gone: %q", store.lastSQL)
	}
}

func TestConflictingL2AnswersNotFoundWhenNothingDisagrees(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeMemoryConflictingL2Request("deploy:target", "staging")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryConflictingL2), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, confidence, decodeErr := db2contract.DecodeMemoryConflictingL2Reply(body)
	if decodeErr != nil || found != 0 || confidence != 0 {
		t.Fatalf("found = %d, confidence = %v", found, confidence)
	}
}

func TestFeatureRowVersionIsPartOfTheKey(t *testing.T) {
	// Features computed by an older extractor are a different row, not a stale
	// one, so a caller asking for a version that has not been computed gets
	// nothing rather than something computed differently.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFeatureRowReadRequest("subject-1", "memory", "v2")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageFeatureRowRead), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "feature_set_version = $3") {
		t.Errorf("the version is no longer part of the lookup: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "ORDER BY") {
		t.Errorf("a newest-version fallback was added: %q", store.lastSQL)
	}
}

func ptrFloat(value float64) *float64 { return &value }

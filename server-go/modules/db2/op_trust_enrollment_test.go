package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestDedupeCandidatesExcludeExpiredAndVersioned(t *testing.T) {
	// Two exclusions ruling out different things: merging into an expired
	// memory would revive it, and a versioned key is one deliberately kept
	// alongside its other versions -- deduplicating those undoes the
	// versioning.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "build-state", 0.9, int64(12), int64(3), 0.7},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryDedupeCandidatesRequest("fact")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryDedupeCandidates), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	candidates, decodeErr := db2contract.DecodeMemoryDedupeCandidatesReply(body)
	if decodeErr != nil || len(candidates) != 1 {
		t.Fatalf("candidates = %+v", candidates)
	}
	if candidates[0].MemoryID != 4 || candidates[0].UseCount != 12 ||
		candidates[0].ObservationCount != 3 || candidates[0].EvidenceStrength != 0.7 {
		t.Fatalf("candidate = %+v", candidates[0])
	}
	if !strings.Contains(store.lastSQL, "COALESCE(valid_until, '') = ''") {
		t.Errorf("an expired memory could be merged into: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "key NOT LIKE '%#v%'") {
		t.Errorf("versioned memories would be deduplicated: %q", store.lastSQL)
	}
}

func TestHotAntiPatternsAreStablyOrdered(t *testing.T) {
	// The C orders by hit count alone, so two patterns hit the same number of
	// times come back in whatever order the planner picked. Confidence breaks
	// the tie.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAntiPatternListHotRequest(5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageAntiPatternListHot), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY hit_count DESC, confidence DESC") {
		t.Errorf("the ordering is no longer stable: %q", store.lastSQL)
	}
	if store.lastArgs[0] != int64(5) {
		t.Errorf("threshold = %v", store.lastArgs[0])
	}
}

func TestTrustBumpsTheEpochOnlyOnARealChange(t *testing.T) {
	// A no-op re-assert should not invalidate the frequency model that reads
	// the epoch -- but it is still audited, with the epoch equal on both sides,
	// which is how the log tells a re-assert from a change.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{"trusted"}},        // the prior trust
		{values: []any{int64(7), "hash"}}, // the epoch and the repo set hash
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCrossRepoSetTrustRequest(
		"aimee", "trusted", "jbailes", "req-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCrossRepoSetTrust), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, prior, changed, decodeErr :=
		db2contract.DecodeCrossRepoSetTrustReply(body)
	if decodeErr != nil || result != trustWritten || prior != "trusted" || changed != 0 {
		t.Fatalf("result = %d, prior = %q, changed = %d", result, prior, changed)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d, want only the audit row", store.execCalls)
	}
	audit := store.argsLog[len(store.argsLog)-1]
	if audit[4] != int64(7) || audit[5] != int64(7) {
		t.Errorf("epochs = %v and %v, want equal on a no-op", audit[4], audit[5])
	}
	if audit[1] != "jbailes" || audit[7] != "req-1" {
		t.Errorf("the audit row lost who asked: %v", audit)
	}
}

func TestTrustChangeWritesBumpsAndAudits(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{"untrusted"}},
		{values: []any{int64(7), "hash"}},
		{values: []any{int64(8)}}, // the bumped epoch
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCrossRepoSetTrustRequest(
		"aimee", "trusted", "jbailes", "req-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCrossRepoSetTrust), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, prior, changed, decodeErr :=
		db2contract.DecodeCrossRepoSetTrustReply(body)
	if decodeErr != nil || result != trustWritten || prior != "untrusted" ||
		changed != 1 {
		t.Fatalf("result = %d, prior = %q, changed = %d", result, prior, changed)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	audit := store.argsLog[len(store.argsLog)-1]
	if audit[4] != int64(7) || audit[5] != int64(8) {
		t.Errorf("epochs = %v and %v, want the transition", audit[4], audit[5])
	}
	// The prior read holds the row, which the C does not: two operators
	// asserting opposite trust at once could otherwise both read the same
	// prior, both call it a change, and bump the epoch twice for one
	// transition.
	if !strings.Contains(store.sqlLog[0], "FOR UPDATE") {
		t.Errorf("the row is not held across the decision: %q", store.sqlLog[0])
	}
}

func TestTrustRefusesAValueNothingUnderstands(t *testing.T) {
	// A trust level nobody recognises would read as untrusted everywhere it is
	// compared, which is a quiet answer to a question an operator asked
	// explicitly.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCrossRepoSetTrustRequest(
		"aimee", "probably", "jbailes", "req-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCrossRepoSetTrust), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, _, changed, decodeErr := db2contract.DecodeCrossRepoSetTrustReply(body)
	if decodeErr != nil || result != trustFailed || changed != 0 {
		t.Fatalf("result = %d, changed = %d", result, changed)
	}
	if store.txCalls != 0 {
		t.Error("a transaction was opened for a value that cannot be written")
	}
}

func TestTrustReportsAMissingProject(t *testing.T) {
	// An operator naming a project that does not exist gets told that rather
	// than told nothing.
	store := &fakeStore{rowQueue: []*fakeRow{{err: pgx.ErrNoRows}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCrossRepoSetTrustRequest(
		"nothing-here", "trusted", "jbailes", "req-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCrossRepoSetTrust), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, prior, changed, decodeErr :=
		db2contract.DecodeCrossRepoSetTrustReply(body)
	if decodeErr != nil || result != trustNoProject || prior != "" || changed != 0 {
		t.Fatalf("result = %d, prior = %q, changed = %d", result, prior, changed)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}

func TestEnrollmentGuardRefusesToResurrectARevokedCertificate(t *testing.T) {
	// The redeem path must never bring back a revoked certificate, and a
	// different certificate must not take over a fingerprint. An existing
	// non-empty issuer or serial still has to match.
	for _, clause := range []string{
		"kb_enrollments.revoked_at = ''",
		"kb_enrollments.cert_issuer = EXCLUDED.cert_issuer",
		"kb_enrollments.cert_serial_norm = EXCLUDED.cert_serial_norm",
	} {
		if !strings.Contains(enrollmentInsertQuery, clause) {
			t.Errorf("missing %s", clause)
		}
	}
	// The empty exceptions are what let a legacy backfill be superseded: a
	// placeholder knows neither issuer nor serial, and requiring equality alone
	// left it permanently un-upgradable.
	if !strings.Contains(enrollmentInsertQuery, "kb_enrollments.cert_issuer = ''") ||
		!strings.Contains(enrollmentInsertQuery, "kb_enrollments.cert_serial_norm = ''") {
		t.Errorf("a backfilled placeholder could never be upgraded: %q",
			enrollmentInsertQuery)
	}
	// The authority is not reset on conflict: it is the anchor the resolve
	// hands out, and changing it would strand whoever holds it.
	update := enrollmentInsertQuery[strings.Index(enrollmentInsertQuery, "DO UPDATE SET"):]
	if strings.Contains(update, "authority_id") {
		t.Errorf("re-enrolment would strand the authority: %q", update)
	}
}

func TestEnrollmentAnswersWhatItWrote(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: []any{int64(31)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentInsertRequest(
		"kb", "fingerprint", "CN=issuer", "01ab", "2027-01-01T00:00:00Z", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEnrollmentInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, enrollmentID, decodeErr :=
		db2contract.DecodeEnrollmentInsertReply(body)
	if decodeErr != nil || acknowledged != 1 || enrollmentID != 31 {
		t.Fatalf("acknowledged = %d, id = %d", acknowledged, enrollmentID)
	}
	// The normalized serial is written to both columns, because the C binds it
	// to both: the pair of issuer and normalized serial is the revocation key,
	// and the plain serial column predates it.
	if store.lastArgs[2] != "01ab" || store.lastArgs[7] != "01ab" {
		t.Errorf("args = %v", store.lastArgs)
	}
}

func TestEnrollmentThatFailsTheGuardIsNotAcknowledged(t *testing.T) {
	// A conflict failing the guard updates nothing and returns no row. The
	// caller must not believe it now owns the enrolment.
	store := &fakeStore{row: &fakeRow{err: errors.New("no rows")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentInsertRequest(
		"kb", "fingerprint", "CN=issuer", "01ab", "", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEnrollmentInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, enrollmentID, decodeErr :=
		db2contract.DecodeEnrollmentInsertReply(body)
	if decodeErr != nil || acknowledged != 0 || enrollmentID != 0 {
		t.Fatalf("acknowledged = %d, id = %d", acknowledged, enrollmentID)
	}
}

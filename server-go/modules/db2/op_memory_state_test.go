package db2

import (
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestDedupeDryRunWritesNothing(t *testing.T) {
	// An operator has to be able to see what a pass would do before letting it,
	// and this pass merges autonomously with nothing gating it.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(4)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDedupeByKeyRequest(1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDedupeByKey), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	merged, decodeErr := db2contract.DecodeDedupeByKeyReply(body)
	if decodeErr != nil || merged != 4 {
		t.Fatalf("merged = %d", merged)
	}
	if store.execCalls != 0 {
		t.Fatalf("statements = %d -- a dry run wrote", store.execCalls)
	}
	if !strings.Contains(store.lastSQL, "SELECT COUNT(*) FROM duplicates") {
		t.Errorf("the dry run did not count the same set: %q", store.lastSQL)
	}
}

func TestDedupeMergesAndRecordsInOneStatement(t *testing.T) {
	// The merge is applied autonomously and nobody reviews it, so the
	// provenance record is the safety mechanism -- a row that acquires
	// merged_into with no trace of when or into which canonical makes an
	// incorrect merge both unnoticeable and un-undoable. One statement means a
	// merge and its record land together or neither does.
	store := &fakeStore{execRowsAt: true, execRows: 3}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDedupeByKeyRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDedupeByKey), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	merged, decodeErr := db2contract.DecodeDedupeByKeyReply(body)
	if decodeErr != nil || merged != 3 {
		t.Fatalf("merged = %d", merged)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d -- the merge and the record can be interrupted "+
			"between them", store.execCalls)
	}
	if !strings.Contains(store.lastSQL, "UPDATE memories SET merged_into") ||
		!strings.Contains(store.lastSQL, "INSERT INTO memory_provenance") {
		t.Errorf("the statement does not both merge and record: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "'dedupe_merge'") {
		t.Error("the provenance action changed; an undo looks for this string")
	}
}

func TestDedupePicksTheMostConfidentAsCanonical(t *testing.T) {
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDedupeByKeyRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDedupeByKey), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "DISTINCT ON (key)") ||
		!strings.Contains(store.lastSQL, "ORDER BY key, confidence DESC") {
		t.Errorf("the canonical row is not the most confident: %q", store.lastSQL)
	}
	// Already-merged rows and L0 are outside the set on both sides of the join;
	// dropping either half would merge a row into one that is itself merged.
	if strings.Count(store.lastSQL, "merged_into = 0") != 2 ||
		strings.Count(store.lastSQL, "tier NOT IN ('L0')") != 2 {
		t.Errorf("the duplicate set is not filtered on both sides: %q", store.lastSQL)
	}
}

func TestEnrollmentTouchNeverRevivesARevokedCertificate(t *testing.T) {
	// The conflict path touches only last_seen_at. Adding state or revoked_at to
	// it would let a revoked certificate resurrect itself by being used, which
	// is the whole security property of this statement.
	enrollmentDebounce.seen = map[string]time.Time{}
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentTouchLastSeenRequest("ab12cd34", "kb")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEnrollmentTouchLastSeen), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	_, conflict, found := strings.Cut(store.lastSQL, "DO UPDATE")
	if !found {
		t.Fatalf("no conflict clause: %q", store.lastSQL)
	}
	if strings.Contains(conflict, "state") || strings.Contains(conflict, "revoked_at") {
		t.Errorf("the conflict path can revive a revoked certificate: %q", conflict)
	}
	if !strings.Contains(conflict, "last_seen_at") {
		t.Errorf("the conflict path does not record the sighting: %q", conflict)
	}
}

func TestEnrollmentTouchDebouncesRepeatSightings(t *testing.T) {
	// Every request on a mutually-authenticated connection touches an
	// enrollment, so without this it is a write per request for a column
	// nothing reads at that resolution.
	enrollmentDebounce.seen = map[string]time.Time{}
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentTouchLastSeenRequest("ab12cd34", "kb")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	for attempt := range 3 {
		body, status := handler(invocation(db2contract.StageEnrollmentTouchLastSeen), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("attempt %d: status = %v", attempt, status)
		}
		// Every attempt reports the sighting recorded, because one inside the
		// window already is.
		recorded, decodeErr := db2contract.DecodeEnrollmentTouchLastSeenReply(body)
		if decodeErr != nil || recorded != 1 {
			t.Fatalf("attempt %d: recorded = %d", attempt, recorded)
		}
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d, want one for three sightings in the window",
			store.execCalls)
	}
}

func TestEnrollmentDebounceWritesAgainAfterTheWindow(t *testing.T) {
	debounce := &fingerprintDebounce{seen: map[string]time.Time{}}
	start := time.Unix(1_800_000_000, 0)
	if !debounce.shouldWrite("ab12cd34", start) {
		t.Fatal("the first sighting did not write")
	}
	if debounce.shouldWrite("ab12cd34", start.Add(enrollmentDebounceWindow-time.Second)) {
		t.Error("a sighting inside the window wrote")
	}
	if !debounce.shouldWrite("ab12cd34", start.Add(enrollmentDebounceWindow)) {
		t.Error("a sighting after the window did not write")
	}
}

func TestEnrollmentDebounceStaysBounded(t *testing.T) {
	// It must not grow with the number of distinct certificates ever seen.
	debounce := &fingerprintDebounce{seen: map[string]time.Time{}}
	start := time.Unix(1_800_000_000, 0)
	for index := range enrollmentDebounceEntries * 3 {
		debounce.shouldWrite(string(rune('a'+index%26))+string(rune(index)), start)
	}
	if len(debounce.seen) > enrollmentDebounceEntries {
		t.Fatalf("entries = %d, want at most %d",
			len(debounce.seen), enrollmentDebounceEntries)
	}
}

func TestAuthorityIDIsAOneTwentyEightBitHexString(t *testing.T) {
	first, err := newAuthorityID()
	if err != nil {
		t.Fatalf("mint: %v", err)
	}
	if len(first) != 32 {
		t.Fatalf("length = %d, want 32 hex characters", len(first))
	}
	second, err := newAuthorityID()
	if err != nil {
		t.Fatalf("mint: %v", err)
	}
	if first == second {
		t.Fatal("two mints produced the same identifier")
	}
}

func TestRetroScanMarkerNamesNoMemories(t *testing.T) {
	// NULL on both sides is what distinguishes a marker from a finding in the
	// ledger they share.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryRetroScanMarkerRequest("2026-01-01T00:00:00Z")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryRetroScanMarker), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "VALUES ($1, NULL, NULL, 'scan', 'retroactive_scan')") {
		t.Errorf("the marker is no longer distinguishable from a finding: %q", store.lastSQL)
	}
}

func TestLifecycleGetStateAnswersEmptyForAnAbsentMemory(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeLifecycleGetStateRequest(2147483000)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLifecycleGetState), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	state, decodeErr := db2contract.DecodeLifecycleGetStateReply(body)
	if decodeErr != nil || state != "" {
		t.Fatalf("state = %q", state)
	}
}

package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestStateFieldsAnswersWhetherAnExpiryExists(t *testing.T) {
	// The caller is deciding whether the memory expires at all, not when. An
	// empty string counts as absent alongside NULL, because the column is
	// written both ways -- and a memory with an empty valid_until that read as
	// expiring would be treated as decaying on a date nobody set.
	for _, probe := range []struct {
		name       string
		validUntil *string
		want       uint32
	}{
		{name: "null", validUntil: nil, want: 0},
		{name: "empty", validUntil: ptr(""), want: 0},
		{name: "set", validUntil: ptr("2026-01-01T00:00:00Z"), want: 1},
	} {
		t.Run(probe.name, func(t *testing.T) {
			store := &fakeStore{row: &fakeRow{values: []any{
				probe.validUntil, int64(3), int64(9),
			}}}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeMemoryStateFieldsRequest(4)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(db2contract.StageMemoryStateFields), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			found, hasValidUntil, observations, uses, decodeErr :=
				db2contract.DecodeMemoryStateFieldsReply(body)
			if decodeErr != nil || found != 1 {
				t.Fatalf("found = %d", found)
			}
			if hasValidUntil != probe.want {
				t.Errorf("has_valid_until = %d, want %d", hasValidUntil, probe.want)
			}
			if observations != 3 || uses != 9 {
				t.Errorf("observations = %d, uses = %d", observations, uses)
			}
		})
	}
}

func TestProvenanceSeparatesAbsentFromFailed(t *testing.T) {
	// A memory that was superseded or deleted has a provenance question with a
	// real answer -- there is none -- and a read that went wrong has no answer
	// at all. Collapsing them would tell a caller that a memory it can see is
	// gone.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryProvenanceByIDRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryProvenanceByID), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, _, _, _, decodeErr := db2contract.DecodeMemoryProvenanceByIDReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if result != provenanceAbsent {
		t.Fatalf("no such memory answered %d, want absent", result)
	}

	store = &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler = NewDispatchHandler(store)
	body, status = handler(invocation(db2contract.StageMemoryProvenanceByID), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, _, _, _, decodeErr = db2contract.DecodeMemoryProvenanceByIDReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if result != provenanceFailed {
		t.Fatalf("a failed read answered %d, want failed", result)
	}
}

func TestProvenanceReadsTheSessionAsNullable(t *testing.T) {
	// A memory can be written by something that is not a session, and that
	// column is the only nullable one of the three.
	store := &fakeStore{row: &fakeRow{values: []any{
		"fact", (*string)(nil), "2026-01-01T00:00:00Z",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryProvenanceByIDRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryProvenanceByID), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, kind, session, version, decodeErr :=
		db2contract.DecodeMemoryProvenanceByIDReply(body)
	if decodeErr != nil || result != provenanceFound {
		t.Fatalf("result = %d", result)
	}
	if kind != "fact" || session != "" || version != "2026-01-01T00:00:00Z" {
		t.Fatalf("kind = %q, session = %q, version = %q", kind, session, version)
	}
}

func TestUnitMetaRefusesUnitsOfDeadMemories(t *testing.T) {
	// "Active" in the name is the tier filter. The unit row carries no tier of
	// its own, so without the join a unit belonging to an archived memory would
	// keep scoring as though the memory were still live.
	store := &fakeStore{row: &fakeRow{values: []any{0.75, "claim", "semantic"}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryUnitActiveMetaRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryUnitActiveMeta), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, weight, unitType, unitKind, decodeErr :=
		db2contract.DecodeMemoryUnitActiveMetaReply(body)
	if decodeErr != nil || found != 1 || weight != 0.75 ||
		unitType != "claim" || unitKind != "semantic" {
		t.Fatalf("found = %d, weight = %v, type = %q, kind = %q",
			found, weight, unitType, unitKind)
	}
	if !strings.Contains(store.lastSQL, "m.tier IN ('L1', 'L2', 'L3')") {
		t.Errorf("a unit of a dead memory could answer here: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "JOIN memories m ON m.id = u.memory_id") {
		t.Errorf("the tier is no longer reachable: %q", store.lastSQL)
	}
}

func TestLifecycleCountsAnswerEveryStateIncludingTheEmptyOnes(t *testing.T) {
	// A grouped read fills only the states it finds, and the reply has five
	// fixed fields. Filtered aggregates always return one row, so a state with
	// no memories reads as zero rather than being left at whatever the previous
	// caller saw.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(5), int64(0), int64(2), int64(0), int64(7),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLifecycleCountsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLifecycleCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	active, pending, fulfilled, superseded, archived, decodeErr :=
		db2contract.DecodeLifecycleCountsReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if active != 5 || pending != 0 || fulfilled != 2 || superseded != 0 || archived != 7 {
		t.Fatalf("counts = %d %d %d %d %d",
			active, pending, fulfilled, superseded, archived)
	}
	if strings.Contains(store.lastSQL, "GROUP BY") {
		t.Errorf("a grouped read would leave absent states unset: %q", store.lastSQL)
	}
}

func TestSetArtifactStoresAnAbsentHashAsNull(t *testing.T) {
	// Not cosmetic. memory_artifact_hashed_list selects on artifact_hash IS NOT
	// NULL, so an empty string here would put an artifact that cannot be checked
	// for drift in front of the verification pass -- which would then compare
	// nothing against nothing and call it verified.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySetArtifactRequest(
		4, "file", "docs/a.md", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemorySetArtifact), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	changed, decodeErr := db2contract.DecodeMemorySetArtifactReply(body)
	if decodeErr != nil || changed != 1 {
		t.Fatalf("changed = %d", changed)
	}
	if !strings.Contains(store.lastSQL, "NULLIF($3, '')") {
		t.Errorf("an empty hash would be stored as an empty string: %q", store.lastSQL)
	}
}

func TestSetArtifactReportsNothingChangedForAnUnknownMemory(t *testing.T) {
	// The changed-row count rather than statement success, so pointing a memory
	// that does not exist at an artifact reads as unchanged.
	store := &fakeStore{execRows: 0, execRowsAt: true}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySetArtifactRequest(
		4, "file", "docs/a.md", "abc123")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemorySetArtifact), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	changed, decodeErr := db2contract.DecodeMemorySetArtifactReply(body)
	if decodeErr != nil || changed != 0 {
		t.Fatalf("changed = %d, want 0", changed)
	}
}

func TestExtractionInsertsToleratePriorExtractions(t *testing.T) {
	// The same entity or the same temporal reference coming out of one memory
	// twice is the expected case, not a collision. There is nothing to update
	// on conflict either -- a second extraction carries the weight the first
	// did.
	for _, probe := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
	}{
		{
			name:  "entity",
			stage: db2contract.StageMemoryEntityInsert,
			build: func() ([]byte, error) {
				return db2contract.EncodeMemoryEntityInsertRequest(4, "postgres", "", 0.5)
			},
		},
		{
			name:  "temporal",
			stage: db2contract.StageMemoryTemporalInsert,
			build: func() ([]byte, error) {
				return db2contract.EncodeMemoryTemporalInsertRequest(4, "last tuesday", "", 0.5)
			},
		},
	} {
		t.Run(probe.name, func(t *testing.T) {
			store := &fakeStore{}
			handler := NewDispatchHandler(store)
			request, err := probe.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(probe.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if !strings.Contains(store.lastSQL, "ON CONFLICT DO NOTHING") {
				t.Errorf("a repeat extraction would fail: %q", store.lastSQL)
			}
			// The role and the granularity are stored as given, empty included.
			// The C substitutes a default for a NULL, and the adapter decodes
			// into a buffer -- so the default is unreachable and the empty
			// string is what lands.
			if store.lastArgs[2] != "" {
				t.Errorf("arg = %v, want the empty string the adapter produces",
					store.lastArgs[2])
			}
		})
	}
}

func TestExtractionInsertsReportAFailedWrite(t *testing.T) {
	// The C acknowledges unconditionally: its backend returns void, so by the
	// time the adapter answers there is nothing left to report. That error is
	// not lost here, and this is the one place the two answers differ.
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryEntityInsertRequest(4, "postgres", "mention", 0.5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryEntityInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMemoryEntityInsertReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d for a failed insert, want 0", acknowledged)
	}
}

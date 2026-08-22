package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestLifecycleUpdateAppliesFourSeparateRules(t *testing.T) {
	// Each CASE is its own rule and they are easy to collapse into one another:
	// leaving pending clears the deadline, entering archived records the reason,
	// entering superseded or archived closes the event-time interval, and that
	// last only when it is still open.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLifecycleUpdateStateRequest(4, "archived", "superseded")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageLifecycleUpdateState), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL,
		"ttl_at = CASE WHEN $2 = 'pending' THEN ttl_at ELSE '' END") {
		t.Errorf("leaving pending no longer clears the deadline: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL,
		"archive_reason = CASE WHEN $2 = 'archived' THEN $3 ELSE archive_reason END") {
		t.Errorf("a non-archiving transition would erase the reason: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "$2 IN ('superseded','archived')") {
		t.Errorf("the event-time interval is no longer closed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, `COALESCE(valid_until, '') = ''`) {
		t.Errorf("a caller-asserted end date would be overwritten by the "+
			"transition stamp: %q", store.lastSQL)
	}
	// One parameter serves all four uses, where the C binds the state four
	// times because its wrapper numbers placeholders positionally.
	if len(store.lastArgs) != 3 {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestAliasInsertKeepsTheFirstWeight(t *testing.T) {
	// DO NOTHING rather than DO UPDATE: re-observing an alias must not raise
	// its weight, or an alias seen often would outrank one seen once with real
	// confidence.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryAliasInsertRequest(4, "pg", 0.8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryAliasInsert), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (memory_id, alias) DO NOTHING") {
		t.Errorf("re-observing an alias now changes its weight: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 3 || store.lastArgs[2] != 0.8 {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestEpisodeCardConflictIncludesTheText(t *testing.T) {
	// A card whose wording changed is a new row rather than an update, so two
	// cards for one memory can coexist -- and the read beside this takes
	// whichever comes first.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryEpisodeCardInsertRequest(4, "key", "what happened")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryEpisodeCardInsert), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL,
		"ON CONFLICT (memory_id, unit_type, unit_key, unit_text) DO NOTHING") {
		t.Errorf("the conflict target changed: %q", store.lastSQL)
	}
	// The weight and the flag belong to the row shape, not the caller: every
	// episode card is weighted the same.
	if !strings.Contains(store.lastSQL, "'episode_card', 'episodic', $2, $3, 2.0, 1") {
		t.Errorf("a card's constants moved: %q", store.lastSQL)
	}
}

func TestEntitiesListTieBreaksOnInsertionOrder(t *testing.T) {
	// Without it the order of equally weighted entities is the planner's to
	// choose, and a caller rendering the first few would see them shuffle
	// between reads.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryEntitiesListRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryEntitiesList), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY weight DESC, id ASC") {
		t.Errorf("the ordering is no longer stable: %q", store.lastSQL)
	}
}

func TestIDKeyContentTreatsZeroAsNoLimit(t *testing.T) {
	// The C appends its LIMIT clause only when the limit is positive, so zero
	// means every memory. A plain LIMIT $1 would answer none.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryIDKeyContentRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryIDKeyContent), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "LIMIT NULLIF($1, 0)") {
		t.Errorf("a zero limit would truncate to nothing: %q", store.lastSQL)
	}
}

func TestEvidenceFieldsSeparateFoundFromZero(t *testing.T) {
	// Strength zero with no observations is a real state -- a memory asserted
	// once and never corroborated -- and a memory nothing holds is another. A
	// caller deciding whether to promote reads them differently.
	t.Run("asserted but never corroborated", func(t *testing.T) {
		zero := 0.0
		store := &fakeStore{row: &fakeRow{values: []any{&zero, idPtr(0)}}}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeMemoryEvidenceFieldsRequest(4)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageMemoryEvidenceFields), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, strength, observations, decodeErr :=
			db2contract.DecodeMemoryEvidenceFieldsReply(body)
		if decodeErr != nil || found != 1 || strength != 0 || observations != 0 {
			t.Fatalf("found = %d, strength = %v, observations = %d",
				found, strength, observations)
		}
	})

	t.Run("a memory nothing holds", func(t *testing.T) {
		handler := NewDispatchHandler(&fakeStore{})
		request, err := db2contract.EncodeMemoryEvidenceFieldsRequest(2147483000)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageMemoryEvidenceFields), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		found, _, _, decodeErr := db2contract.DecodeMemoryEvidenceFieldsReply(body)
		if decodeErr != nil || found != 0 {
			t.Fatalf("found = %d, want 0", found)
		}
	})
}

func TestDirectiveCountsReportZeroForAnEmptyState(t *testing.T) {
	// The reply has four fixed fields. A grouped scan fills them by matching
	// names it may not find, which leaves a state at whatever it held before;
	// filtered aggregates always answer all four.
	store := &fakeStore{row: &fakeRow{values: []any{
		idPtr(2), idPtr(0), idPtr(5), idPtr(0),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDirectiveCountsByStateRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDirectiveCountsByState), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	open, suppressed, resolved, expired, decodeErr :=
		db2contract.DecodeDirectiveCountsByStateReply(body)
	if decodeErr != nil || open != 2 || suppressed != 0 || resolved != 5 || expired != 0 {
		t.Fatalf("open = %d, suppressed = %d, resolved = %d, expired = %d",
			open, suppressed, resolved, expired)
	}
	// The same four names the C matches, and nothing else: a directive in a
	// fifth state is counted in none of them.
	for _, state := range []string{"'open'", "'suppressed'", "'resolved'", "'expired'"} {
		if !strings.Contains(store.lastSQL, state) {
			t.Errorf("%s is no longer counted: %q", state, store.lastSQL)
		}
	}
}

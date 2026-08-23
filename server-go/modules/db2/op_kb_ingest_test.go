package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestFenceReadSplitsIdentityAtTheFirstSpace(t *testing.T) {
	// The generation and the purge identifier share one column, separated by a
	// space, and the split is at the first one. A purge identifier containing a
	// space therefore survives intact; a generation containing one does not.
	// That asymmetry is the C's, and a caller comparing the identifier it holds
	// against what comes back depends on it.
	store := &fakeStore{row: &fakeRow{values: []any{
		ptr("gen-7 purge id with spaces"), true,
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBPurgeFenceReadRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBPurgeFenceRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	present, generation, purgeID, liveness, decodeErr :=
		db2contract.DecodeKBPurgeFenceReadReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if present != 1 || liveness != 1 {
		t.Fatalf("present = %d, live = %d", present, liveness)
	}
	if generation != "gen-7" {
		t.Errorf("generation = %q", generation)
	}
	if purgeID != "purge id with spaces" {
		t.Errorf("purge id = %q -- the split took more than the first space", purgeID)
	}
}

func TestFenceReadAsksLivenessAgainstAThirdOfTheTTL(t *testing.T) {
	// TTL/3, not the TTL. The owning purge heartbeats at least every TTL/6, so
	// this is twice the heartbeat interval -- the point at which silence means
	// something. Reading liveness against the full TTL would call a fence live
	// for minutes after its owner had stopped, and this answer feeds a takeover
	// decision.
	InstallRuntimeConfig(RuntimeConfig{KBPurgeFenceTTLSeconds: 900})
	defer InstallRuntimeConfig(RuntimeConfig{})

	store := &fakeStore{row: &fakeRow{values: []any{ptr("gen-1 purge-1"), false}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBPurgeFenceReadRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageKBPurgeFenceRead), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 3 {
		t.Fatalf("args = %v", store.lastArgs)
	}
	if store.lastArgs[2] != 300 {
		t.Errorf("liveness window = %v seconds, want a third of the TTL", store.lastArgs[2])
	}
	if !strings.Contains(store.lastSQL, "make_interval(secs => $3)") {
		t.Errorf("the window is no longer a parameter: %q", store.lastSQL)
	}
}

func TestFenceTTLFallsBackWhenUnconfigured(t *testing.T) {
	// Zero means unset, not "expire immediately". A misconfiguration that
	// treated it as a real value would make every fence read as dead, which is
	// the failure that hands one project to two purges at once.
	InstallRuntimeConfig(RuntimeConfig{})
	if got := kbPurgeFenceTTLSeconds(); got != 900 {
		t.Fatalf("unconfigured TTL = %d, want the default", got)
	}
	InstallRuntimeConfig(RuntimeConfig{KBPurgeFenceTTLSeconds: -1})
	defer InstallRuntimeConfig(RuntimeConfig{})
	if got := kbPurgeFenceTTLSeconds(); got != 900 {
		t.Fatalf("negative TTL = %d, want the default", got)
	}
}

func TestFenceReadAnswersAbsentRatherThanFailing(t *testing.T) {
	// No fence is the common answer, not an error: the caller has learned that
	// nothing holds the project either way.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBPurgeFenceReadRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBPurgeFenceRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	present, generation, purgeID, liveness, decodeErr :=
		db2contract.DecodeKBPurgeFenceReadReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if present != 0 || liveness != 0 || generation != "" || purgeID != "" {
		t.Fatalf("present = %d, live = %d, generation = %q, purge = %q",
			present, liveness, generation, purgeID)
	}
}

func TestFileIndexPinsTheGeneration(t *testing.T) {
	// The caller uses the returned hash to decide whether to re-ingest. A hit
	// from a superseded generation, or from a project that has been detached,
	// means the content never gets re-read -- so both halves of the pinning are
	// load-bearing and neither is redundant with the other.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("abc123"), ptr("2026-01-01T00:00:00Z")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBFileIndexGetRequest("replay-project", "docs/a.md")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBFileIndexGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, hash, ingestedAt, decodeErr := db2contract.DecodeKBFileIndexGetReply(body)
	if decodeErr != nil || found != 1 || hash != "abc123" ||
		ingestedAt != "2026-01-01T00:00:00Z" {
		t.Fatalf("found = %d, hash = %q, at = %q", found, hash, ingestedAt)
	}
	if !strings.Contains(store.lastSQL, "k.generation = p.current_generation") {
		t.Errorf("a superseded generation could answer: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "p.lifecycle_state = 'current'") {
		t.Errorf("a detached project could answer: %q", store.lastSQL)
	}
}

func TestEmptyProjectNeverReachesEitherOperation(t *testing.T) {
	// The C checks for an empty project in both of these and answers without
	// reading. The envelope refuses one first -- both fields declare a minimum
	// length of one -- so the check has moved rather than been dropped, and
	// neither operation carries a branch that cannot be reached.
	if _, err := db2contract.EncodeKBFileIndexGetRequest("", "docs/a.md"); err == nil {
		t.Error("an empty project encoded for the file index read")
	}
	if _, err := db2contract.EncodeKBPurgeFenceReadRequest(""); err == nil {
		t.Error("an empty project encoded for the fence read")
	}
}

func TestIngestCompletionRecordsWhatTheJobDid(t *testing.T) {
	// The counts land in the same statement as the state change, so a job never
	// reads as done with nothing to show for it.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBIngestQueueCompleteRequest(42, 7, 90, 88)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBIngestQueueComplete), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBIngestQueueCompleteReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "status = 'done'") ||
		!strings.Contains(store.lastSQL, "completed_at = pg_now_text()") {
		t.Errorf("the completion no longer marks the job done: %q", store.lastSQL)
	}
	want := []any{int64(42), int64(7), int64(90), int64(88)}
	for index, expected := range want {
		if store.lastArgs[index] != expected {
			t.Fatalf("args = %v, want %v -- the counts are transposed",
				store.lastArgs, want)
		}
	}
}

func TestDocStateClearsTheReviewOnlyWhenAsked(t *testing.T) {
	// A document can move between states while still needing review, so
	// clearing is opt-in. A state change that dismissed the review by itself
	// would lose the reason someone raised it.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocSetStateRequest(9, "published", 0, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageKBDocSetState), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "review_needed") {
		t.Errorf("the review flag was touched unasked: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "review_reason") {
		t.Errorf("the review reason was overwritten unasked: %q", store.lastSQL)
	}
}

func TestDocStateClearingWritesTheReasonAlongsideTheFlag(t *testing.T) {
	// The C has a third branch that clears the flag and leaves the reason,
	// reachable only through a NULL pointer the module never produces -- the
	// adapter decodes into a buffer, so an absent reason arrives as the empty
	// string. Writing the empty reason with the cleared flag is what happens
	// today, and pinning it here is what would catch the branch being
	// resurrected as a behaviour change.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBDocSetStateRequest(9, "published", 1, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBDocSetState), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBDocSetStateReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "review_needed = false") ||
		!strings.Contains(store.lastSQL, "review_reason = $2") {
		t.Errorf("clearing no longer writes both: %q", store.lastSQL)
	}
	if store.lastArgs[1] != "" {
		t.Errorf("reason = %v, want the empty string the adapter produces",
			store.lastArgs[1])
	}
}

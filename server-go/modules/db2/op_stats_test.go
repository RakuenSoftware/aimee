package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestProjectStatsExcludesDotPaths(t *testing.T) {
	// These are the numbers shown to a person as "what is indexed". A .git
	// directory is not something they put there, and counting it would make
	// every project look larger than the work in it.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(12), idPtr(40)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectStatsRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	files, definitions, decodeErr := db2contract.DecodeProjectStatsReply(body)
	if decodeErr != nil || files != 12 || definitions != 40 {
		t.Fatalf("files = %d, definitions = %d", files, definitions)
	}
	// Both halves, since dropping the exclusion from one would leave the two
	// numbers describing different file sets.
	if strings.Count(store.lastSQL, `f.path NOT LIKE '.%'`) != 2 ||
		strings.Count(store.lastSQL, `f.path NOT LIKE '%/.%'`) != 2 ||
		strings.Count(store.lastSQL, `p.root NOT LIKE '%/.%'`) != 2 {
		t.Errorf("the dot-path exclusion is not applied to both counts: %q", store.lastSQL)
	}
}

func TestProjectStatsAnswersZeroForAnUnindexedProject(t *testing.T) {
	// SUM and COUNT differ here: COUNT over no rows is zero, so a project with
	// nothing indexed answers zero rather than NULL. The pointer scan covers
	// the case anyway, because a subquery over a missing project can still
	// answer NULL.
	store := &fakeStore{row: &fakeRow{values: []any{(*int64)(nil), (*int64)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectStatsRequest("never-indexed")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	files, definitions, decodeErr := db2contract.DecodeProjectStatsReply(body)
	if decodeErr != nil || files != 0 || definitions != 0 {
		t.Fatalf("files = %d, definitions = %d, want zero and zero", files, definitions)
	}
}

func TestPurgeFilesMatchingPassesThePatternThrough(t *testing.T) {
	// The caller is purging a directory or an extension and needs its
	// wildcards. Escaping them here would take away the only thing this does.
	store := &fakeStore{execRowsAt: true, execRows: 9}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodePurgeFilesMatchingRequest(4, "vendor/%")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StagePurgeFilesMatching), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	deleted, decodeErr := db2contract.DecodePurgeFilesMatchingReply(body)
	if decodeErr != nil || deleted != 9 {
		t.Fatalf("deleted = %d, want the count", deleted)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "vendor/%" {
		t.Fatalf("args = %v -- the pattern was altered", store.lastArgs)
	}
	if !strings.Contains(store.lastSQL,
		"generation = (SELECT current_generation FROM projects WHERE id = $1)") {
		t.Errorf("the purge is not scoped to the current generation: %q", store.lastSQL)
	}
}

func TestSettledCountsReachTheirWindowFallback(t *testing.T) {
	// committed_at is NOT NULL with an empty-string default, so a bare COALESCE
	// always returned it and the empty string never satisfied the window
	// comparison. Every archived proposal was excluded, which made the terminal
	// count identical to the committed one and the second reply field dead.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(3), idPtr(11)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProposalsSettledCountsRequest(30)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProposalsSettledCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	committed, terminal, decodeErr := db2contract.DecodeProposalsSettledCountsReply(body)
	if decodeErr != nil || committed != 3 || terminal != 11 {
		t.Fatalf("committed = %d, terminal = %d", committed, terminal)
	}
	if !strings.Contains(store.lastSQL, "NULLIF(committed_at, '')") ||
		!strings.Contains(store.lastSQL, "NULLIF(updated_at, '')") {
		t.Errorf("the window fallback is unreachable again: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != "-30 days" {
		t.Fatalf("args = %v -- the window is not an interval expression", store.lastArgs)
	}
}

func TestSettledCountsSurviveAnEmptyWindow(t *testing.T) {
	// SUM over no rows is NULL, not zero. Scanning into plain integers would
	// fail the whole read for a quiet month.
	store := &fakeStore{row: &fakeRow{values: []any{(*int64)(nil), (*int64)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProposalsSettledCountsRequest(7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProposalsSettledCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	committed, terminal, decodeErr := db2contract.DecodeProposalsSettledCountsReply(body)
	if decodeErr != nil || committed != 0 || terminal != 0 {
		t.Fatalf("committed = %d, terminal = %d, want zero and zero", committed, terminal)
	}
}

func TestRetrievalEventByTurnAnswersEmptyWhenTheTurnRetrievedNothing(t *testing.T) {
	// Ordinary rather than exceptional: a turn that answered without retrieving
	// has no event.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeRetrievalEventByTurnRequest("turn-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRetrievalEventByTurn), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	eventID, payload, decodeErr := db2contract.DecodeRetrievalEventByTurnReply(body)
	if decodeErr != nil || eventID != "" || payload != "" {
		t.Fatalf("id = %q, payload = %q", eventID, payload)
	}
}

func TestRetrievalEventByTurnToleratesANullTurnID(t *testing.T) {
	// artifacts.turn_id arrives by ALTER and is nullable, so both selected
	// columns scan through pointers.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("event-1"), (*string)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRetrievalEventByTurnRequest("turn-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRetrievalEventByTurn), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	eventID, payload, decodeErr := db2contract.DecodeRetrievalEventByTurnReply(body)
	if decodeErr != nil || eventID != "event-1" || payload != "" {
		t.Fatalf("id = %q, payload = %q", eventID, payload)
	}
}

func TestArtifactLinksReadFollowsOneDirection(t *testing.T) {
	// This answers what an artifact points at, not what points at it. Nothing
	// on this wire answers the reverse, which is worth knowing before assuming
	// the graph is walkable both ways.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("artifact-2"), ptr("supersedes")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactLinksReadRequest("artifact-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactLinksRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeArtifactLinksReadReply(body)
	if decodeErr != nil || len(found) != 1 || found[0].ToArtifactID != "artifact-2" {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "WHERE from_id = $1") ||
		strings.Contains(store.lastSQL, "to_id = $1") {
		t.Errorf("the direction changed: %q", store.lastSQL)
	}
}

func TestStageCountsGroupByStageAndStatus(t *testing.T) {
	// A stage nothing has reached is absent rather than zero: this reports the
	// pairs that have jobs, and there is no list of stages to fill in against.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("extract"), ptr("pending"), idPtr(4)},
		{ptr("extract"), ptr("failed"), idPtr(1)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCorpusPipelineStageCountsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCorpusPipelineStageCounts), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeCorpusPipelineStageCountsReply(body)
	if decodeErr != nil || len(found) != 2 || found[0].JobCount != 4 {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "GROUP BY stage, stage_status") ||
		!strings.Contains(store.lastSQL, "ORDER BY stage, stage_status") {
		t.Errorf("the grouping or ordering changed: %q", store.lastSQL)
	}
}

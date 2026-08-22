package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestArtifactCiteRecordsAWholeSourceSpan(t *testing.T) {
	// (0, 0) is read specially by artifact_invalidate_citing: it means the whole
	// source, so a citation written here is invalidated by any change to it.
	// Writing any other pair would narrow what invalidates the artifact.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactCiteRequest("artifact-1", "kb_document", "42")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageArtifactCite), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "VALUES ($1, $2, $3, 0, 0)") {
		t.Errorf("the citation no longer covers the whole source: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT DO NOTHING") {
		t.Errorf("re-running the work would now be a constraint violation: %q",
			store.lastSQL)
	}
}

func TestCitationAndLinkAreRepeatable(t *testing.T) {
	// Recording the same citation twice is a caller re-running work, not a
	// mistake to surface, and the reply cannot tell the two apart anyway.
	for _, testCase := range []struct {
		name   string
		stage  uint32
		build  func() ([]byte, error)
		decode func([]byte) (uint32, error)
	}{
		{
			"artifact_cite",
			db2contract.StageArtifactCite,
			func() ([]byte, error) {
				return db2contract.EncodeArtifactCiteRequest("artifact-1", "kb_document", "42")
			},
			db2contract.DecodeArtifactCiteReply,
		},
		{
			"artifact_link",
			db2contract.StageArtifactLink,
			func() ([]byte, error) {
				return db2contract.EncodeArtifactLinkRequest(
					"artifact-1", "artifact-2", "supersedes")
			},
			db2contract.DecodeArtifactLinkReply,
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{execRowsAt: true, execRows: 0}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			acknowledged, decodeErr := testCase.decode(body)
			if decodeErr != nil || acknowledged != 1 {
				t.Fatalf("acknowledged = %d -- a row count crept into the answer",
					acknowledged)
			}
		})
	}
}

func TestCollabProposeCapsTheWholeTableInTheStatement(t *testing.T) {
	// Unlike the active cap this bounds every row, proposals included, so a
	// flood of proposals cannot crowd out the ability to propose. Reading the
	// count and then inserting is a check that has stopped being true by the
	// time it is acted on.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCollabRuleProposeRequest(
		"a replay rule", "a replay reason", "replay")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCollabRulePropose), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeCollabRuleProposeReply(body)
	if decodeErr != nil || id != 7 {
		t.Fatalf("id = %d", id)
	}
	if !strings.Contains(store.lastSQL, "WHERE (SELECT COUNT(*) FROM collab_rules) < $4") {
		t.Errorf("the cap is not part of the statement: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 4 || store.lastArgs[3] != maxTotalCollabRules {
		t.Fatalf("args = %v -- the cap is not bound", store.lastArgs)
	}
	if maxTotalCollabRules != 50 {
		t.Fatalf("the total cap is %d; COLLAB_MAX_TOTAL_RULES is 50",
			maxTotalCollabRules)
	}
}

func TestCollabProposeAnswersZeroWhenTheTableIsFull(t *testing.T) {
	// INSERT ... SELECT with a gating WHERE inserts nothing, so RETURNING
	// yields no row rather than an error.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeCollabRuleProposeRequest("full", "full", "replay")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCollabRulePropose), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeCollabRuleProposeReply(body)
	if decodeErr != nil || id != 0 {
		t.Fatalf("id = %d, want 0", id)
	}
}

func TestTaskUpdateStateRequiresTheTask(t *testing.T) {
	// The reply is named "changed", not "acknowledged", and it means a row
	// moved. A caller updating a deleted task is working from a stale list.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeTaskUpdateStateRequest(4, "done")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageTaskUpdateState), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	changed, decodeErr := db2contract.DecodeTaskUpdateStateReply(body)
	if decodeErr != nil || changed != 0 {
		t.Fatalf("changed = %d for a task nothing holds, want 0", changed)
	}
}

func TestDemotionProfileFallsBackNarrowestFirst(t *testing.T) {
	// A profile set for one project must beat the one set for every project,
	// and falling back only when nothing more specific exists is what makes a
	// global default a default rather than an override.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{(*string)(nil)}}, // no exact-scope profile
		{values: []any{(*string)(nil)}}, // none for the scope kind either
		{values: []any{ptr(`{"global":true}`)}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionProfileReadRequest("fact", "project", "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionProfileRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	profile, decodeErr := db2contract.DecodeDemotionProfileReadReply(body)
	if decodeErr != nil || profile != `{"global":true}` {
		t.Fatalf("profile = %q", profile)
	}
	if len(store.argsLog) != 3 {
		t.Fatalf("lookups = %d, want exact, scope-kind and global", len(store.argsLog))
	}
	if store.argsLog[0][2] != "aimee" || store.argsLog[1][2] != "" ||
		store.argsLog[2][1] != "global" {
		t.Fatalf("the fallback order is wrong: %v", store.argsLog)
	}
}

func TestDemotionProfileStopsAtTheFirstMatch(t *testing.T) {
	// The narrowest wins, so a match must not be overwritten by a broader one.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{ptr(`{"exact":true}`)}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionProfileReadRequest("fact", "project", "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionProfileRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	profile, decodeErr := db2contract.DecodeDemotionProfileReadReply(body)
	if decodeErr != nil || profile != `{"exact":true}` {
		t.Fatalf("profile = %q", profile)
	}
	if len(store.argsLog) != 1 {
		t.Fatalf("lookups = %d -- a broader scope was consulted after a match",
			len(store.argsLog))
	}
}

func TestDemotionProfileStampsWhatItReturns(t *testing.T) {
	// The C touches the artifact after reading it, and that stamp feeds the
	// decay sweep: without it a profile in daily use decays as though nobody
	// wanted it. Doing it in the same statement also closes the gap where the
	// C returns a profile and then fails to stamp it.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{ptr("{}")}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionProfileReadRequest("fact", "project", "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDemotionProfileRead), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "last_accessed_at = CURRENT_TIMESTAMP") {
		t.Errorf("the profile is read without being stamped as used: %q", store.lastSQL)
	}
}

func TestFindPendingIgnoresDecidedProposals(t *testing.T) {
	// One already committed or archived has been decided, and a new
	// observation about the same target deserves a new proposal rather than
	// reopening a closed one.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(4)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalFindPendingRequest("rules", "key", 9)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLearningProposalFindPending), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	id, decodeErr := db2contract.DecodeLearningProposalFindPendingReply(body)
	if decodeErr != nil || id != 4 {
		t.Fatalf("id = %d", id)
	}
	if !strings.Contains(store.lastSQL, "state = 'pending'") {
		t.Errorf("a decided proposal could now be corroborated: %q", store.lastSQL)
	}
	// Newest first, so where duplicates already exist the most recent is the
	// one corroborated and the older ones are left to expire.
	if !strings.Contains(store.lastSQL, "ORDER BY id DESC LIMIT 1") {
		t.Errorf("the newest duplicate is no longer chosen: %q", store.lastSQL)
	}
}

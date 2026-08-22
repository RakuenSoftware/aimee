package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestMinhashDeleteFileClearsSignatureAndBucketsTogether(t *testing.T) {
	// The C deletes the signature and then calls lsh_bucket_delete_file, with
	// nothing wrapping the pair. A bucket naming a signature that is gone is not
	// a partly-cleared index, it is a wrong one, and the next similarity read
	// uses it.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMinhashDeleteFileRequest("replay-project", "src/main.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMinhashDeleteFile), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMinhashDeleteFileReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if len(store.sqlLog) != 2 ||
		!strings.Contains(store.sqlLog[0], "kb_minhash_signatures") ||
		!strings.Contains(store.sqlLog[1], "kb_lsh_buckets") {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	// Both scoped by subquery, so a caller cannot name a published generation.
	for index, args := range store.argsLog {
		if len(args) != 2 {
			t.Fatalf("statement %d bound %v; the generation must not come from "+
				"the caller", index, args)
		}
	}
}

func TestMinhashDeleteFileRollsBackWhenTheBucketsFail(t *testing.T) {
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMinhashDeleteFileRequest("replay-project", "src/main.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMinhashDeleteFile), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeMinhashDeleteFileReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack || store.committed {
		t.Fatalf("rolled back = %v, committed = %v", store.rolledBack, store.committed)
	}
}

func TestUniqueFileBasenameRefusesToGuessBetweenTwo(t *testing.T) {
	// The caller is resolving a name a person typed. Two candidates mean the
	// name did not identify a file, and returning the first would be picking one
	// on their behalf without saying so.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("src/a/main.c")}, {ptr("src/b/main.c")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeUniqueFileBasenameRequest("replay-project", "main.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageUniqueFileBasename), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	path, decodeErr := db2contract.DecodeUniqueFileBasenameReply(body)
	if decodeErr != nil || path != "" {
		t.Fatalf("path = %q, want empty for an ambiguous basename", path)
	}
}

func TestUniqueFileBasenameAnswersTheOnlyMatch(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{{ptr("src/only/main.c")}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeUniqueFileBasenameRequest("replay-project", "main.c")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageUniqueFileBasename), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	path, decodeErr := db2contract.DecodeUniqueFileBasenameReply(body)
	if decodeErr != nil || path != "src/only/main.c" {
		t.Fatalf("path = %q", path)
	}
	// The basename comparison happens in the database rather than over every
	// path in the project, and the limit is two because the question is whether
	// there is exactly one.
	if !strings.Contains(store.lastSQL, `regexp_replace(f.path, '^.*/', '') = $2`) {
		t.Errorf("the basename is not matched in SQL: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "LIMIT 2") {
		t.Error("the read no longer stops at the second candidate")
	}
	if !strings.Contains(store.lastSQL, "f.generation = p.current_generation") {
		t.Error("the read is not bound to the current generation")
	}
}

func TestEntityEdgeBumpTouchesBothSidesAndClampsInSQL(t *testing.T) {
	// The evidence is about the entity, not about one relation it appears in,
	// so every edge naming it on either side moves. The clamp is in the
	// statement so two concurrent bumps each add to whatever is there and
	// neither can push past the bound.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeBumpUtilityRequest("postgres", 0.5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageEntityEdgeBumpUtility), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "WHERE source = $1 OR target = $1") {
		t.Errorf("only one side of the edge is matched: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "GREATEST(-5.0, LEAST(5.0, utility_score + $2))") {
		t.Errorf("the clamp left the statement: %q", store.lastSQL)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != 0.5 {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestArtifactFlagReviewMergesRatherThanReplaces(t *testing.T) {
	// The C reads the payload, deletes the two keys, adds them back, and writes
	// the whole document. The jsonb || operator is that, in one statement --
	// and it closes a race the C's transaction could not, since two concurrent
	// flags there interleave between the SELECT and the UPDATE and the later
	// write discards whatever the earlier merged.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactFlagReviewRequest("artifact-1", "looks wrong")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactFlagReview), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactFlagReviewReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d -- the read and the write can be interleaved",
			store.execCalls)
	}
	if !strings.Contains(store.lastSQL, "||") ||
		!strings.Contains(store.lastSQL, "jsonb_build_object") {
		t.Errorf("the payload is replaced rather than merged: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "jsonb_typeof(payload) = 'object'") {
		t.Error("a payload that is not an object would break the merge")
	}
	if !strings.Contains(store.lastSQL, "state = 'proposed'") {
		t.Error("flagging no longer returns the artifact to proposed")
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "looks wrong" {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestArtifactFlagReviewDefaultsTheReason(t *testing.T) {
	// The column is what a reviewer reads, and an empty string tells them
	// nothing about why they are here.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactFlagReviewRequest("artifact-1", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageArtifactFlagReview), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[1] != "flagged" {
		t.Fatalf("args = %v, want the default reason", store.lastArgs)
	}
}

func TestArtifactFlagReviewNeedsTheArtifactToExist(t *testing.T) {
	// Acknowledging for an artifact nobody holds would tell a caller their flag
	// landed somewhere.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactFlagReviewRequest("no-such-artifact", "why")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactFlagReview), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactFlagReviewReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
}

func TestVerdictSuppressedIsAFlagNotACount(t *testing.T) {
	// One thumbs-down suppresses: no threshold, no decay. Returning the count
	// would invite a caller to start treating "how many" as a strength.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeVerdictSuppressedRequest("tag", "scope")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageVerdictSuppressed), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	suppressed, decodeErr := db2contract.DecodeVerdictSuppressedReply(body)
	if decodeErr != nil || suppressed != 1 {
		t.Fatalf("suppressed = %d, want 1 for seven refusals", suppressed)
	}
	if !strings.Contains(store.lastSQL, "verdict = 'thumbs_down'") {
		t.Errorf("the read no longer counts refusals: %q", store.lastSQL)
	}
}

func TestVerdictSuppressedIsFalseWithoutARefusal(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(0)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeVerdictSuppressedRequest("tag", "scope")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageVerdictSuppressed), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	suppressed, decodeErr := db2contract.DecodeVerdictSuppressedReply(body)
	if decodeErr != nil || suppressed != 0 {
		t.Fatalf("suppressed = %d, want 0", suppressed)
	}
}

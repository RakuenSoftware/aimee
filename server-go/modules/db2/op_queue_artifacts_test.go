package db2

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestQueueTotalCountsStatusesNobodyNamed(t *testing.T) {
	// The total is a plain count rather than the sum of the four named
	// statuses. A job in a status nobody named still exists, and a total that
	// only counted the known ones would quietly under-report the queue.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(2), int64(1), int64(5), int64(0), int64(11),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBAsyncQueueStatusRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBAsyncQueueStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	pending, running, done, failed, total, processed, decodeErr :=
		db2contract.DecodeKBAsyncQueueStatusReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if pending != 2 || running != 1 || done != 5 || failed != 0 {
		t.Fatalf("counts = %d %d %d %d", pending, running, done, failed)
	}
	if total != 11 {
		t.Errorf("total = %d, want every job including the unnamed three", total)
	}
	// queue_processed is always zero: the C's backend never writes it, so the
	// adapter has been encoding a zero since the field was added.
	if processed != 0 {
		t.Errorf("processed = %d, want the zero the C answers", processed)
	}
}

func TestClaimSkipsRowsAnotherWorkerHolds(t *testing.T) {
	// SKIP LOCKED is what lets several workers share the queue. Without it a
	// second worker blocks on the first's row and then claims it anyway when
	// the lock clears.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(7), "aimee", "/src/aimee", "default", int64(1),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBIngestQueueClaimNextRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBIngestQueueClaimNext), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	claimed, jobID, project, root, workspace, force, decodeErr :=
		db2contract.DecodeKBIngestQueueClaimNextReply(body)
	if decodeErr != nil || claimed != 1 || jobID != 7 || project != "aimee" ||
		root != "/src/aimee" || workspace != "default" || force != 1 {
		t.Fatalf("claim = %d %d %q %q %q %d",
			claimed, jobID, project, root, workspace, force)
	}
	if !strings.Contains(store.lastSQL, "FOR UPDATE SKIP LOCKED") {
		t.Errorf("two workers could claim one job: %q", store.lastSQL)
	}
	// Priority first, then arrival order within a priority, so a bulk reindex
	// cannot starve work a caller is blocked on.
	if !strings.Contains(store.lastSQL, "ORDER BY priority DESC, id") {
		t.Errorf("the queue is no longer prioritised: %q", store.lastSQL)
	}
	// Claim and mark in one statement: the row moves to running before anyone
	// else can see it, which is what makes it a claim rather than a read.
	if !strings.Contains(store.lastSQL, "SET status = 'running'") ||
		!strings.Contains(store.lastSQL, "RETURNING") {
		t.Errorf("the claim is no longer atomic: %q", store.lastSQL)
	}
}

func TestEmptyQueueIsNotAFailure(t *testing.T) {
	// A worker polling an idle queue gets this answer constantly.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBIngestQueueClaimNextRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBIngestQueueClaimNext), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	claimed, jobID, _, _, _, _, decodeErr :=
		db2contract.DecodeKBIngestQueueClaimNextReply(body)
	if decodeErr != nil || claimed != 0 || jobID != 0 {
		t.Fatalf("claimed = %d, job = %d", claimed, jobID)
	}
}

func TestVectorOpStampsTheVersionOnlyOnSuccess(t *testing.T) {
	// A failed attempt produced no vector, so claiming one exists at the
	// current version would be the lie the column was added to stop. On the
	// conflict path a successful re-index restamps, because a point
	// re-embedded at a new version is at the new version.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeVectorIndexOpRecordRequest(
		900, "kb_documents", 4, 1, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageVectorIndexOpRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	recorded, decodeErr := db2contract.DecodeVectorIndexOpRecordReply(body)
	if decodeErr != nil || recorded != 1 {
		t.Fatalf("recorded = %d", recorded)
	}
	if !strings.Contains(store.lastSQL, "SELECT version FROM memory_active_embedder") {
		t.Errorf("the version no longer comes from the table: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL,
		"embedding_version = CASE WHEN excluded.status = 'ok'") {
		t.Errorf("a failed re-index would restamp the version: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "attempts   = vector_index_ops.attempts + 1") {
		t.Errorf("the attempt count no longer grows: %q", store.lastSQL)
	}
	if store.lastArgs[3] != "ok" {
		t.Errorf("status = %v", store.lastArgs[3])
	}
}

func TestVectorOpKeepsTheErrorOnlyForAFailure(t *testing.T) {
	// Carrying an error alongside a success would leave the last failure's
	// text attached to a point that is now fine.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeVectorIndexOpRecordRequest(
		900, "kb_documents", 0, 1, "the previous attempt timed out")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageVectorIndexOpRecord), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[4] != "" {
		t.Errorf("a success carries an error message: %v", store.lastArgs[4])
	}

	store = &fakeStore{}
	handler = NewDispatchHandler(store)
	request, err = db2contract.EncodeVectorIndexOpRecordRequest(
		900, "kb_documents", 0, 0, "the embedder refused")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageVectorIndexOpRecord), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[3] != "failed" || store.lastArgs[4] != "the embedder refused" {
		t.Errorf("args = %v", store.lastArgs)
	}
	// A memory of zero is not a memory, and the column is nullable for it.
	if !strings.Contains(store.lastSQL, "NULLIF($3, 0)") {
		t.Errorf("memory zero would be stored as a memory: %q", store.lastSQL)
	}
}

func TestRejectAndItsAuditLandTogether(t *testing.T) {
	// The C does these as two calls and gives up after the first if the second
	// fails, which leaves an artifact rejected with nothing recording why. The
	// audit trail is the whole point of a rejection carrying a verdict.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactRejectRequest(
		"artifact-1", "wrong-scope", "project", `it broke "everything"`, `{"state":"proposed"}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactReject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactRejectReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed || len(store.sqlLog) != 2 {
		t.Fatalf("transactions = %d, statements = %v", store.txCalls, store.sqlLog)
	}
	if !strings.Contains(store.sqlLog[0], "state = 'rejected'") {
		t.Errorf("the artifact is not rejected first: %q", store.sqlLog[0])
	}
	// The counter-example is free text and will contain quotes, so the
	// after-snapshot is marshalled rather than assembled by hand.
	afterJSON, ok := store.argsLog[1][3].(string)
	if !ok {
		t.Fatalf("after snapshot = %v", store.argsLog[1][3])
	}
	var after map[string]string
	if jsonErr := json.Unmarshal([]byte(afterJSON), &after); jsonErr != nil {
		t.Fatalf("the snapshot is not JSON: %v (%s)", jsonErr, afterJSON)
	}
	if after["state"] != "rejected" || after["verdict_tag"] != "wrong-scope" ||
		after["counter_example"] != `it broke "everything"` {
		t.Fatalf("after = %v", after)
	}
}

func TestRejectLeavesAbsentVerdictFieldsOut(t *testing.T) {
	// An absent verdict tag is different from one someone set to the empty
	// string, and only one of those is worth recording.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactRejectRequest("artifact-1", "", "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageArtifactReject), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	afterJSON, _ := store.argsLog[1][3].(string)
	var after map[string]string
	if jsonErr := json.Unmarshal([]byte(afterJSON), &after); jsonErr != nil {
		t.Fatalf("the snapshot is not JSON: %v", jsonErr)
	}
	if len(after) != 1 || after["state"] != "rejected" {
		t.Fatalf("after = %v", after)
	}
	// An absent before-snapshot arrives as an empty string, and an empty string
	// is not valid JSON. Casting it directly fails the statement, which in the
	// C means the rejection lands and the audit event does not.
	if !strings.Contains(store.sqlLog[1], "NULLIF($3, '')::jsonb") {
		t.Errorf("an absent snapshot would fail the whole write: %q", store.sqlLog[1])
	}
}

func TestRejectRollsBackWhenTheAuditFails(t *testing.T) {
	store := &fakeStore{execErr: errors.New("connection lost")}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactRejectRequest("artifact-1", "", "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactReject), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactRejectReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if !store.rolledBack {
		t.Error("the rejection was left standing without its audit event")
	}
}

func TestFidelityKeepsMalformedApartFromEmpty(t *testing.T) {
	// A report saying nothing was supported and nothing was unsupported is a
	// legitimate answer for a turn that made no claims. A caller cannot tell
	// that from a report it failed to parse unless the two are kept apart.
	for _, probe := range []struct {
		name    string
		payload *fakeRow
		want    uint32
	}{
		{name: "absent", payload: nil, want: fidelityAbsent},
		{
			name:    "malformed",
			payload: &fakeRow{values: []any{"not json at all"}},
			want:    fidelityMalformed,
		},
		{
			name:    "empty report",
			payload: &fakeRow{values: []any{`{"status":"clean"}`}},
			want:    fidelityFound,
		},
	} {
		t.Run(probe.name, func(t *testing.T) {
			store := &fakeStore{row: probe.payload}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeFidelityReportByTurnRequest("turn-1")
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(
				invocation(db2contract.StageFidelityReportByTurn), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			result, _, supported, unsupported, abstained, decodeErr :=
				db2contract.DecodeFidelityReportByTurnReply(body)
			if decodeErr != nil {
				t.Fatalf("decode reply: %v", decodeErr)
			}
			if result != probe.want {
				t.Fatalf("result = %d, want %d", result, probe.want)
			}
			if supported != 0 || unsupported != 0 || abstained != 0 {
				t.Fatalf("counts = %d %d %d", supported, unsupported, abstained)
			}
		})
	}
}

func TestFidelityReadsTheLatestJudgement(t *testing.T) {
	// A turn can be judged more than once -- a re-run of the judge writes
	// another report -- and the latest verdict is the one that counts.
	store := &fakeStore{row: &fakeRow{values: []any{
		`{"status":"unsupported","supported":3,"unsupported":2,"abstained":1}`,
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFidelityReportByTurnRequest("turn-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFidelityReportByTurn), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	result, reportStatus, supported, unsupported, abstained, decodeErr :=
		db2contract.DecodeFidelityReportByTurnReply(body)
	if decodeErr != nil || result != fidelityFound || reportStatus != "unsupported" ||
		supported != 3 || unsupported != 2 || abstained != 1 {
		t.Fatalf("result = %d, status = %q, counts = %d %d %d",
			result, reportStatus, supported, unsupported, abstained)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY created_at DESC LIMIT 1") {
		t.Errorf("an older judgement could answer: %q", store.lastSQL)
	}
}

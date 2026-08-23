package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestPipelineStatusCountsSkippedFromTheEventLog(t *testing.T) {
	// A job row only knows where a document got to, not how much of the journey
	// was a no-op. The C's note is emphatic: a drain reporting fourteen stages
	// processed and none failed looked like a fully processed document and was
	// in fact eight no-ops.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(20), int64(3), int64(1), int64(2), int64(14), int64(8),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCorpusPipelineStatusRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCorpusPipelineStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	total, pending, running, failed, complete, processed, skipped, decodeErr :=
		db2contract.DecodeCorpusPipelineStatusReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if total != 20 || pending != 3 || running != 1 || failed != 2 || complete != 14 {
		t.Fatalf("counts = %d %d %d %d %d", total, pending, running, failed, complete)
	}
	if skipped != 8 {
		t.Errorf("skipped = %d, want the event log's count", skipped)
	}
	// Always zero: the C's status call never sets it -- only the drain does, on
	// the same struct.
	if processed != 0 {
		t.Errorf("processed = %d, want the zero the C answers", processed)
	}
	if !strings.Contains(store.lastSQL, "corpus_stage_events WHERE outcome = 'skipped'") {
		t.Errorf("skipped no longer comes from the event log: %q", store.lastSQL)
	}
	// One statement, so the two counts are consistent with each other: the C
	// reads them separately and a job advancing between the calls is counted in
	// one and not the other.
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v, want one", store.sqlLog)
	}
}

func TestProjectStatusCountsBothVectorCollectionNames(t *testing.T) {
	// The writer records general-corpus vectors under 'kb_embeddings';
	// 'kb_chunks' is the older name and still appears in existing stores.
	// Matching only the legacy one made this count zero on every current
	// deployment, and three separate surfaces then told operators the embedder
	// was broken while it was working.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(120), int64(48000), int64(9), int64(118),
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBProjectStatusRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBProjectStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, project, files, chunks, tokens, embeddings, decodeErr :=
		db2contract.DecodeKBProjectStatusReply(body)
	if decodeErr != nil || found != 1 || project != "aimee" || files != 9 ||
		chunks != 120 || tokens != 48000 || embeddings != 118 {
		t.Fatalf("status = %d %q %d %d %d %d",
			found, project, files, chunks, tokens, embeddings)
	}
	if !strings.Contains(store.lastSQL, "q.collection IN ('kb_chunks', 'kb_embeddings')") {
		t.Errorf("one of the two collection names is gone: %q", store.lastSQL)
	}
	// Files distinct, chunks not: a file is chunked into many documents, and
	// the two numbers together are what say whether ingestion is producing
	// sensible pieces.
	if !strings.Contains(store.lastSQL, "COUNT(DISTINCT d.file_path)") {
		t.Errorf("files are no longer counted distinctly: %q", store.lastSQL)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v, want one", store.sqlLog)
	}
}

func TestProjectStatusKeepsTheWholeCorpusClauseItCannotReach(t *testing.T) {
	// The statement reads an empty name as every project, which is the C's
	// whole-corpus answer. Through the module it is unreachable: the envelope
	// declares a minimum length of one for the name. The clause is kept rather
	// than dropped because it is the statement's, not the operation's -- the
	// same text runs under a caller that does not go through the envelope.
	if _, err := db2contract.EncodeKBProjectStatusRequest(""); err == nil {
		t.Error("an empty project name encoded")
	}
	if !strings.Contains(kbProjectStatusQuery, "($1 = '' OR d.project = $1)") {
		t.Errorf("the whole-corpus reading is gone: %q", kbProjectStatusQuery)
	}
}

func TestReembedStatusSeparatesNoRunFromAnEmptyOne(t *testing.T) {
	// A run that has done nothing yet and no run at all encode identically in
	// the counts.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReembedStatusRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReembedStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	haveJob, _, _, total, done, _, _, decodeErr :=
		db2contract.DecodeKBReembedStatusReply(body)
	if decodeErr != nil || haveJob != 0 || total != 0 || done != 0 {
		t.Fatalf("have = %d, total = %d, done = %d", haveJob, total, done)
	}

	store = &fakeStore{row: &fakeRow{values: []any{
		"v2", int64(0), int64(0), int64(0), "2026-01-01T00:00:00Z", "",
	}}}
	handler = NewDispatchHandler(store)
	body, status = handler(invocation(db2contract.StageKBReembedStatus), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	haveJob, targetVersion, _, total, done, startedAt, finishedAt, decodeErr :=
		db2contract.DecodeKBReembedStatusReply(body)
	if decodeErr != nil || haveJob != 1 || targetVersion != "v2" ||
		total != 0 || done != 0 {
		t.Fatalf("have = %d, target = %q", haveJob, targetVersion)
	}
	// A run in progress has not finished, and the column is nullable for it.
	if startedAt == "" || finishedAt != "" {
		t.Fatalf("started = %q, finished = %q", startedAt, finishedAt)
	}
}

func TestReleaseReadAnswersTheTimestampsThatApply(t *testing.T) {
	// A draft release has been neither promoted nor retired, and both columns
	// are nullable for it -- which pgx will not scan into a plain string.
	store := &fakeStore{row: &fakeRow{values: []any{
		"2026.01", "draft", "", "", "2026-01-01T00:00:00Z",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBReleaseReadRequest(4)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBReleaseRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, name, state, promoted, retired, created, decodeErr :=
		db2contract.DecodeKBReleaseReadReply(body)
	if decodeErr != nil || found != 1 || name != "2026.01" || state != "draft" ||
		promoted != "" || retired != "" || created == "" {
		t.Fatalf("release = %d %q %q %q %q %q",
			found, name, state, promoted, retired, created)
	}
	if !strings.Contains(store.lastSQL, "COALESCE(promoted_at, '')") ||
		!strings.Contains(store.lastSQL, "COALESCE(retired_at, '')") {
		t.Errorf("a draft release would fail to scan: %q", store.lastSQL)
	}
}

func TestMiningJobCarriesItsResumePoint(t *testing.T) {
	// The high water mark is what makes a mining job resumable: it is the last
	// row the job consumed, so the next run starts there rather than re-reading
	// everything.
	// enabled is a BOOLEAN column, so the fake presents a bool: an int64 here
	// would let a scan pass in the test that fails against Postgres, which is
	// how this operation came to answer "no such job" for a job that was there.
	store := &fakeStore{row: &fakeRow{values: []any{
		"2026-01-01T00:00:00Z", int64(4210), int64(900), true, "",
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMiningJobGetRequest("interaction-mining")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMiningJobGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, lastRun, highWater, interval, enabled, lastError, decodeErr :=
		db2contract.DecodeMiningJobGetReply(body)
	if decodeErr != nil || found != 1 || lastRun == "" || highWater != 4210 ||
		interval != 900 || enabled != 1 || lastError != "" {
		t.Fatalf("job = %d %q %d %d %d %q",
			found, lastRun, highWater, interval, enabled, lastError)
	}
	if !strings.Contains(store.lastSQL, "COALESCE(last_run_at, '')") ||
		!strings.Contains(store.lastSQL, "COALESCE(last_error, '')") {
		t.Errorf("a job that has never run would fail to scan: %q", store.lastSQL)
	}
}

func TestMiningJobReportsAnUnknownJob(t *testing.T) {
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMiningJobGetRequest("nothing-here")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMiningJobGet), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, _, highWater, _, enabled, _, decodeErr :=
		db2contract.DecodeMiningJobGetReply(body)
	if decodeErr != nil || found != 0 || highWater != 0 || enabled != 0 {
		t.Fatalf("found = %d, high water = %d, enabled = %d",
			found, highWater, enabled)
	}
}

func TestQueueViewLeadsWithActiveWork(t *testing.T) {
	// A queue view is read to answer what is happening now, and a completed job
	// from an hour ago is not that.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"aimee", "running", "2026-01-01T00:00:00Z", int64(3), int64(40), ""},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBIngestQueueRecentRequest(20)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBIngestQueueRecent), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	jobs, decodeErr := db2contract.DecodeKBIngestQueueRecentReply(body)
	if decodeErr != nil || len(jobs) != 1 || jobs[0].JobStatus != "running" ||
		jobs[0].FilesIndexed != 3 || jobs[0].ChunksAdded != 40 {
		t.Fatalf("jobs = %+v", jobs)
	}
	if !strings.Contains(store.lastSQL,
		"CASE status WHEN 'running' THEN 0 WHEN 'pending' THEN 1 ELSE 2 END") {
		t.Errorf("active work no longer leads: %q", store.lastSQL)
	}
	// The timestamp falls back through completion, start and queueing, so every
	// row has one whatever state it is in -- and NULLS LAST keeps a row with
	// none of the three from leading a descending sort, which is where
	// PostgreSQL would otherwise put it.
	if !strings.Contains(store.lastSQL, "COALESCE(completed_at, started_at, queued_at)") {
		t.Errorf("a queued job would sort with no timestamp: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "DESC NULLS LAST") {
		t.Errorf("a row with no timestamp would lead: %q", store.lastSQL)
	}
}

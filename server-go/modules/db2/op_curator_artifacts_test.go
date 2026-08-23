package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestAgentOutcomesAreAppendOnly(t *testing.T) {
	// The table is read as a distribution -- how often this agent fails, how
	// many turns it usually takes -- and deduplicating would make the common
	// case invisible.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAgentOutcomeRecordRequest(
		"reviewer", "review", "succeeded", "", 4, 12, 90000, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAgentOutcomeRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeAgentOutcomeRecordReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if strings.Contains(store.lastSQL, "ON CONFLICT") {
		t.Errorf("identical runs would collapse: %q", store.lastSQL)
	}
	if store.lastArgs[6] != int64(90000) {
		t.Errorf("tokens = %v", store.lastArgs[6])
	}
}

func TestAntiPatternMatchingIsWordBounded(t *testing.T) {
	// Boundaries are what stop a pattern like "rm" matching "warm", and an
	// empty pattern never matches -- a zero-length row would otherwise match
	// everything, which is the failure mode of a table anyone can write to.
	if phraseMatches("rm", "the water is warm") {
		t.Error("a pattern matched inside a word")
	}
	if !phraseMatches("rm -rf", "please do not rm -rf /") {
		t.Error("a bounded phrase failed to match")
	}
	if phraseMatches("", "anything at all") {
		t.Error("an empty pattern matched")
	}
	if !phraseMatches("rm", "rm") {
		t.Error("a whole-target match failed")
	}
	if phraseMatches("longer than target", "short") {
		t.Error("a pattern longer than the target matched")
	}
}

func TestAntiPatternNormalisationMakesBothSidesComparable(t *testing.T) {
	// A pattern written with a newline in it should match a command typed with
	// a space, which is what normalising both sides is for.
	if got := normalizeForMatch("  RM\t-RF\n\n/tmp  "); got != "rm -rf /tmp" {
		t.Fatalf("normalised to %q", got)
	}
	if !phraseMatches(normalizeForMatch("RM\n-RF"), normalizeForMatch("do rm -rf now")) {
		t.Error("normalisation did not make the two comparable")
	}
}

func TestAntiPatternCheckJoinsPathAndCommand(t *testing.T) {
	// A pattern can name either or both: one catches the command, another
	// catches an edit to a path.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), "rm -rf", "destroys work", "review", "pr/1", int64(2), 0.9},
		{int64(5), "never matches", "", "", "", int64(0), 0.5},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAntiPatternCheckRequest(
		"src/a.c", "rm -rf /tmp/build")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAntiPatternCheck), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	tripped, decodeErr := db2contract.DecodeAntiPatternCheckReply(body)
	if decodeErr != nil || len(tripped) != 1 || tripped[0].AntiPatternID != 4 {
		t.Fatalf("tripped = %+v", tripped)
	}
	// Matching happens in Go: a word-bounded phrase comparison over normalised
	// text is not something a LIKE can express.
	if strings.Contains(store.lastSQL, "WHERE") {
		t.Errorf("the filter moved into SQL: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY confidence DESC") {
		t.Errorf("the most confident patterns no longer lead: %q", store.lastSQL)
	}
}

func TestProposedArtifactsCarryTheirCitationsInOneStatement(t *testing.T) {
	// The C runs one statement per row it returned -- fifty for a full page.
	// The aggregate is one, and its ordering matters because the joined string
	// is what callers compare.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"artifact-1", "synthesis", "recall", 0.9, "2026-01-01T00:00:00Z",
			`{"claim":"x"}`, "chunk-1,chunk-2"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactListProposedRequest("recall", 20)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactListProposed), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	proposed, decodeErr := db2contract.DecodeArtifactListProposedReply(body)
	if decodeErr != nil || len(proposed) != 1 ||
		proposed[0].CitationIds != "chunk-1,chunk-2" ||
		proposed[0].ArtifactKind != "synthesis" {
		t.Fatalf("proposed = %+v", proposed)
	}
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v, want one", store.sqlLog)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY c.source_kind, c.source_id") {
		t.Errorf("the joined citations would vary between runs: %q", store.lastSQL)
	}
	if store.lastArgs[1] != int64(20) {
		t.Errorf("limit = %v", store.lastArgs[1])
	}
	// The C's default for a caller that asks for nothing is unreachable through
	// the envelope, which bounds the limit at one. It is kept because it
	// belongs to the operation rather than to the wire.
	if _, encodeErr := db2contract.EncodeArtifactListProposedRequest("recall", 0); encodeErr == nil {
		t.Error("a limit of zero encoded")
	}
}

func TestProposedListWithoutASurfaceSeesEverything(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactListProposedRequest("", 10)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageArtifactListProposed), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "($1 = '' OR a.target_surface = $1)") {
		t.Errorf("an empty surface no longer means every surface: %q", store.lastSQL)
	}
	if store.lastArgs[1] != int64(10) {
		t.Errorf("limit = %v", store.lastArgs[1])
	}
}

func TestArtifactWriteKeepsTheCallersIdentifier(t *testing.T) {
	// Unlike the writes that mint one, this identifier is the caller's -- so a
	// retry with the same identifier writes one artifact.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactWriteRequest(
		"artifact-1", "synthesis", "", "", "aimee", "jbailes", 0.8, `{"claim":"x"}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactWrite), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactWriteReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.lastArgs[0] != "artifact-1" {
		t.Errorf("the identifier was replaced: %v", store.lastArgs[0])
	}
	if store.lastArgs[2] != "proposed" || store.lastArgs[3] != "user" {
		t.Errorf("the defaults changed: %v", store.lastArgs)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (id) DO NOTHING") {
		t.Errorf("a retry would fail rather than collapse: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "$8::jsonb") {
		t.Errorf("the payload is not cast for a JSONB column: %q", store.lastSQL)
	}
}

func TestInvalidationIsAllOrNothing(t *testing.T) {
	// The C's loop can fail part-way and leave a document half-invalidated --
	// some artifacts stale, others still claiming to describe content that has
	// changed. That is worse than not having run, because it looks like it did.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(3)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidateDocRequest("aimee", "docs/a.md")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCuratorInvalidateDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	invalidated, decodeErr := db2contract.DecodeCuratorInvalidateDocReply(body)
	if decodeErr != nil || invalidated != 3 {
		t.Fatalf("invalidated = %d", invalidated)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	// Two statements: the chained CTE that does the work, and the event that
	// records it happened.
	if len(store.sqlLog) != 2 {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if !strings.Contains(store.sqlLog[0], "UPDATE artifacts SET state = 'stale'") ||
		!strings.Contains(store.sqlLog[0], "INSERT INTO audit_events") {
		t.Errorf("the stale and the audit are no longer one statement: %q",
			store.sqlLog[0])
	}
	if !strings.Contains(store.sqlLog[0], "a.state IN ('proposed', 'committed')") {
		t.Errorf("an already-retired artifact would be re-staled: %q", store.sqlLog[0])
	}
	if store.argsLog[1][1] != int64(3) {
		t.Errorf("the event records %v stale artifacts", store.argsLog[1][1])
	}
}

func TestInvalidatingNothingRecordsNothing(t *testing.T) {
	// A file re-ingested with nothing citing it is not news.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(0)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidateDocRequest("aimee", "docs/a.md")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCuratorInvalidateDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	invalidated, decodeErr := db2contract.DecodeCuratorInvalidateDocReply(body)
	if decodeErr != nil || invalidated != 0 {
		t.Fatalf("invalidated = %d", invalidated)
	}
	if store.execCalls != 0 {
		t.Errorf("an invalidation event was written for nothing")
	}
}

func TestInvalidationFailureAnswersZero(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{{err: errors.New("connection lost")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCuratorInvalidateDocRequest("aimee", "docs/a.md")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCuratorInvalidateDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	invalidated, decodeErr := db2contract.DecodeCuratorInvalidateDocReply(body)
	if decodeErr != nil || invalidated != 0 {
		t.Fatalf("invalidated = %d, want 0", invalidated)
	}
	if !store.rolledBack {
		t.Error("the transaction was not rolled back")
	}
}

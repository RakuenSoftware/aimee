package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

// errCorpusStageRead stands in for a stage handler's read failing.
var errCorpusStageRead = errors.New("the document could not be read")

// corpusStatusRow is the six counts the drain reads at the end.
func corpusStatusRow() *fakeRow {
	return &fakeRow{values: []any{
		int64(4), int64(1), int64(0), int64(1), int64(2), int64(9),
	}}
}

func drainCorpus(t *testing.T, store *fakeStore, limit uint32) (
	[]uint32, bus.ModuleStatus,
) {
	t.Helper()
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCorpusPipelineDrainRequest(limit)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCorpusPipelineDrain), request)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	drained, total, pending, running, failed, complete, processed, skipped,
		decodeErr := db2contract.DecodeCorpusPipelineDrainReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	return []uint32{drained, total, pending, running, failed, complete,
		processed, skipped}, status
}

func TestDrainStopsWhenThereIsNothingToMove(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},
		corpusStatusRow(),
	}}
	reply, status := drainCorpus(t, store, 5)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if reply[0] != 1 {
		t.Errorf("drained = %d, want the acknowledgement", reply[0])
	}
	if reply[6] != 0 || reply[7] != 0 {
		t.Errorf("processed = %d, skipped = %d, want an idle drain",
			reply[6], reply[7])
	}
	// The counts are the corpus's, read once at the end.
	if reply[1] != 4 || reply[4] != 1 || reply[5] != 2 {
		t.Errorf("counts = %v", reply)
	}
	if !strings.Contains(store.sqlLog[0], "FOR UPDATE SKIP LOCKED") {
		t.Errorf("two drains could take the same job: %q", store.sqlLog[0])
	}
}

func TestDrainSkipsAStageNothingLocalCanRun(t *testing.T) {
	// "chunked" has no handler here. A stage this process cannot run is not a
	// stage nothing can, so it is skipped rather than failed.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), "sectioned"}},
		{err: pgx.ErrNoRows},
		corpusStatusRow(),
	}}
	reply, status := drainCorpus(t, store, 2)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if reply[6] != 1 || reply[7] != 1 {
		t.Fatalf("processed = %d, skipped = %d", reply[6], reply[7])
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "INSERT INTO corpus_stage_events") {
		t.Fatalf("the skip was not recorded: %q", statements)
	}
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "INSERT INTO corpus_stage_events") {
			if store.argsLog[index][3] != "skipped" ||
				store.argsLog[index][4] != "no local handler" {
				t.Errorf("event = %v", store.argsLog[index])
			}
			if store.argsLog[index][2] != "chunked" {
				t.Errorf("to stage = %v", store.argsLog[index][2])
			}
		}
	}
}

func TestDrainMarksCompleteAtTheLastStage(t *testing.T) {
	// The last step of all: there is no handler for "complete", so it is a
	// skip, and the job's status becomes complete rather than pending.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), "gaps_detected"}},
		corpusStatusRow(),
	}}
	if _, status := drainCorpus(t, store, 1); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	advanced := false
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "UPDATE corpus_processing_jobs\n SET stage =") {
			advanced = true
			if store.argsLog[index][0] != "complete" ||
				store.argsLog[index][1] != "complete" {
				t.Errorf("advance = %v", store.argsLog[index])
			}
		}
	}
	if !advanced {
		t.Fatal("the job never advanced")
	}
}

func TestDrainHonoursTheStepLimit(t *testing.T) {
	store := &fakeStore{
		row: &fakeRow{values: []any{int64(7), "sectioned"}},
		rowQueue: []*fakeRow{
			{values: []any{int64(7), "sectioned"}},
			{values: []any{int64(7), "sectioned"}},
			corpusStatusRow(),
		},
	}
	reply, status := drainCorpus(t, store, 2)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if reply[6] != 2 {
		t.Fatalf("processed = %d, want the limit", reply[6])
	}
	if store.txCalls != 2 {
		t.Errorf("steps = %d", store.txCalls)
	}
}

func TestDrainReportsWhatItDidWhenAStageFails(t *testing.T) {
	// The C returns a bare failure here and throws away everything it already
	// processed, so a drain of forty documents that met one bad handler
	// reported nothing at all -- indistinguishable from a drain that did
	// nothing.
	store := &fakeStore{
		rowQueue: []*fakeRow{
			{values: []any{int64(7), "sectioned"}},
			{values: []any{int64(8), "ingested"}},
			{err: errCorpusStageRead},
			corpusStatusRow(),
		},
	}
	reply, status := drainCorpus(t, store, 5)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if reply[6] != 1 {
		t.Fatalf("processed = %d, want the step that worked", reply[6])
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "SET stage_status = 'failed'") {
		t.Errorf("the failed job was left pending: %q", statements)
	}
	if !store.rolledBack {
		t.Error("a failed stage was left half-applied")
	}
}

func TestNextStageWalksTheLadder(t *testing.T) {
	if next, ok := corpusNextStage(""); !ok || next != "classified" {
		t.Errorf("empty -> %q", next)
	}
	if next, ok := corpusNextStage("gaps_detected"); !ok || next != "complete" {
		t.Errorf("gaps -> %q", next)
	}
	if _, ok := corpusNextStage("complete"); ok {
		t.Error("complete has a successor")
	}
	// "restore" is parked deliberately: it is reachable only through the
	// restoration path and does not rejoin the ladder.
	if _, ok := corpusNextStage("restore"); ok {
		t.Error("restore rejoined the pipeline")
	}
}

func TestClassifierRanksTheRulesInOrder(t *testing.T) {
	for _, testCase := range []struct {
		filename   string
		text       string
		kind       string
		confidence float64
	}{
		{"main.go", "# architecture", "code", 1.0},
		{"notes.md", "```python\nprint()\n", "code", 1.0},
		{"adr-004.md", "", "architecture", 0.9},
		{"design.md", "", "design", 0.82},
		{"notes.md", "## approach", "design", 0.82},
		{"spec.md", "", "spec", 0.78},
		{"notes.md", "acceptance criteria", "spec", 0.78},
		{"runbook.md", "", "implementation", 0.72},
		{"notes.md", "nothing in particular", "other", 0.55},
	} {
		kind, confidence := classifyCorpusDocType(testCase.filename,
			testCase.text)
		if kind != testCase.kind || confidence != testCase.confidence {
			t.Errorf("%q/%q -> %q %.2f", testCase.filename, testCase.text,
				kind, confidence)
		}
	}
	// A file that is code is code whatever else its text says: the first rule
	// wins, and the ladder's order is the classifier.
	if kind, _ := classifyCorpusDocType("architecture.go", ""); kind != "code" {
		t.Errorf("the ladder reordered: %q", kind)
	}
}

func TestSectionsOwnTheSpansTheyActuallyHave(t *testing.T) {
	text := "intro\n# One\nbody\n## Two\nmore\n# Three\ntail\n"
	sections := parseCorpusSections(text)
	if len(sections) != 3 {
		t.Fatalf("sections = %+v", sections)
	}
	// The C writes each section with a span running to the end of the document
	// and corrects it later, so a rebuild that failed part way leaves every
	// section claiming the whole document.
	one := sections[0]
	if one.SpanStart != 6 || one.SpanEnd != int64(strings.Index(text, "# Three")) {
		t.Errorf("first section = %+v", one)
	}
	two := sections[1]
	if two.Depth != 2 || two.ParentIndex != 0 ||
		two.HeadingPath != "One > Two" {
		t.Errorf("nested section = %+v", two)
	}
	// A depth-one heading closes what is under it, so the next depth-one
	// section starts a fresh ordinal at depth two.
	three := sections[2]
	if three.Ordinal != 2 || three.ParentIndex != -1 {
		t.Errorf("third section = %+v", three)
	}
}

func TestHeadingsMustLookLikeHeadings(t *testing.T) {
	// A run of hashes with no space, or with nothing after it, is something a
	// document says rather than a section it has.
	if sections := parseCorpusSections("#nospace\n## \n####### deep\n"); len(
		sections) != 0 {
		t.Fatalf("sections = %+v", sections)
	}
}

func TestReferencesAreFoundInBothSpellings(t *testing.T) {
	references := scanCorpusReferences(
		"a [link](docs/one.md), and see docs/two.md.\n")
	if len(references) != 2 {
		t.Fatalf("references = %+v", references)
	}
	// The trailing punctuation the prose left is stripped, and the two
	// patterns carry different confidence: a bare mention is a weaker claim
	// than a link.
	if references[0].Target != "docs/one.md" ||
		references[0].Confidence != 1.0 {
		t.Errorf("link = %+v", references[0])
	}
	if references[1].Target != "docs/two.md" ||
		references[1].Type != "mentions" || references[1].Confidence != 0.75 {
		t.Errorf("mention = %+v", references[1])
	}
}

func TestARepeatedReferenceIsOneReference(t *testing.T) {
	// The C counts insert attempts, so a document linking the same target
	// twice claims two references where it has one.
	references := scanCorpusReferences("[a](one.md) and [b](one.md)\n")
	if len(references) != 1 {
		t.Fatalf("references = %+v", references)
	}
	// A word with no dot in it is not a filename.
	if found := scanCorpusReferences("see nothing here\n"); len(found) != 0 {
		t.Fatalf("references = %+v", found)
	}
}

func TestTermsAreTakenFromNamesAndQuotedSpans(t *testing.T) {
	terms := collectCorpusTerms("the `scope guard` splits ParseHeading here")
	if len(terms) != 3 || terms[0] != "scope guard" || terms[1] != "Parse" ||
		terms[2] != "Heading" {
		t.Fatalf("terms = %v", terms)
	}
	// The canonical form loses the whitespace rather than collapsing it,
	// which is the C's spelling and what the existing mappings were built
	// from.
	if canonical := canonicalCorpusTerm("Scope Guard"); canonical !=
		"scopeguard" {
		t.Errorf("canonical = %q", canonical)
	}
}

func TestTermPayloadsSurviveAQuote(t *testing.T) {
	// A term is lifted verbatim out of a quoted span, so a term containing a
	// quote is not unusual -- and the C's snprintf-ed payload would not be
	// JSON, which the JSONB column rejects.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), `he said "Hello There" loudly`, "hash", ""}},
	}}
	store.rowQueue[0].values = []any{int64(7), "doc.md", "hash",
		"the `say \\\"hi\\\"` helper"}
	store.rows = &fakeRows{}
	detail, err := normalizeCorpusTerms(t.Context(), store, 7)
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if detail != "terms=1" {
		t.Fatalf("detail = %q", detail)
	}
	for index, sql := range store.sqlLog {
		if !strings.Contains(sql, "INSERT INTO artifacts") {
			continue
		}
		payloads, ok := store.argsLog[index][1].([]string)
		if !ok || len(payloads) != 1 {
			t.Fatalf("payloads = %#v", store.argsLog[index][1])
		}
		if !strings.Contains(payloads[0], `\"`) {
			t.Errorf("the quote was not escaped: %s", payloads[0])
		}
	}
}

func TestGapsAreRaisedOnceAndPromoted(t *testing.T) {
	store := &fakeStore{
		rowsQueue: []*fakeRows{
			{values: [][]any{{"entity-1"}, {"entity-2"}}},
			{values: [][]any{{"entity-1"}}},
			{values: [][]any{{int64(4), "docs/missing.md"}}},
			{values: [][]any{}},
		},
	}
	detail, err := detectCorpusGaps(t.Context(), store, 7)
	if err != nil {
		t.Fatalf("detect: %v", err)
	}
	// entity-2 and the dangling reference: entity-1 was already raised.
	if detail != "gaps=2" {
		t.Fatalf("detail = %q", detail)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "INSERT INTO curiosity_items") {
		t.Fatalf("nothing was promoted: %q", statements)
	}
	// A dangling reference is weak coverage, not a missing fact: the corpus
	// points at something it does not hold.
	gapTypes := []string{}
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "INSERT INTO curiosity_items") {
			gapTypes = append(gapTypes, store.argsLog[index][0].(string))
		}
	}
	if len(gapTypes) != 2 || gapTypes[0] != "missing_fact" ||
		gapTypes[1] != "weak_coverage" {
		t.Fatalf("gap types = %v", gapTypes)
	}
	// The same absence noticed twice is one question.
	if !strings.Contains(statements, "existing.state NOT IN ('resolved','suppressed')") {
		t.Errorf("a subject could collect a second item: %q", statements)
	}
}

package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// errTypedFactWrite stands in for the insert failing under a scripted store.
var errTypedFactWrite = errors.New("typed fact write failed")

// cssSignalsRow scripts one exemplar scan: rules, token declarations, BEM
// rules, in the order the query selects them.
func cssSignalsRow(rules, tokens, bem int64) *fakeRow {
	return &fakeRow{values: []any{rules, tokens, bem}}
}

// installFlags points both CSS flags at the given value for one test and puts
// the snapshot back afterwards, so a test that switches a feature off cannot
// leave it off for the next one.
func installFlags(t *testing.T, cssStyleGraph, typedFacts bool) {
	t.Helper()
	InstallRuntimeConfig(RuntimeConfig{
		CSSStyleGraphEnabled: &cssStyleGraph,
		TypedFactsEnabled:    &typedFacts,
	})
	t.Cleanup(func() { InstallRuntimeConfig(RuntimeConfig{}) })
}

func TestBothCSSFlagsDefaultToOn(t *testing.T) {
	// The C's defaults table says on for both, and an unset flag here has to
	// mean the deployment did not say -- not that it said no.
	InstallRuntimeConfig(RuntimeConfig{})
	if !cssStyleGraphEnabled() || !typedFactsEnabled() {
		t.Fatal("an unconfigured deployment lost a feature it never disabled")
	}
}

func TestEnumerateRefreshesCoverageWithoutResettingState(t *testing.T) {
	// A unit somebody is midway through converting keeps its state: the
	// enumerate exists to refresh how much of it is resolved, not to decide
	// again whether it has been started.
	store := &fakeStore{execRows: 7, execRowsAt: true}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationEnumerateRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssMigrationEnumerate), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	enumerated, decodeErr := db2contract.DecodeCssMigrationEnumerateReply(body)
	if decodeErr != nil || enumerated != 7 {
		t.Fatalf("enumerated = %d", enumerated)
	}
	if strings.Contains(store.lastSQL, "SET state") ||
		strings.Contains(store.lastSQL, "oracle_equivalent =") {
		t.Errorf("an in-flight unit would be reset: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "SET total_tokens = EXCLUDED.total_tokens") {
		t.Errorf("coverage is no longer refreshed: %q", store.lastSQL)
	}
	// One statement: the C's scan-then-upsert-per-row writes coverage a scan
	// has already made stale.
	if store.execCalls != 1 {
		t.Errorf("statements = %d, want one", store.execCalls)
	}
}

func TestEnumerateTakesTheSameUnitsEachRun(t *testing.T) {
	// The C's GROUP BY has no ordering, so a project with more files than the
	// cap enumerated whichever units the plan happened to emit -- a different
	// set each run.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationEnumerateRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCssMigrationEnumerate), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY f.path") {
		t.Errorf("the unit order is arbitrary again: %q", store.lastSQL)
	}
	if store.lastArgs[1] != cssMigrationUnitCap {
		t.Errorf("cap = %v, want the reply field's ceiling", store.lastArgs[1])
	}
}

func TestRulesDocCarriesTheExemplarsOwnNumbers(t *testing.T) {
	store := &fakeStore{row: cssSignalsRow(12, 5, 3)}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationRulesDocRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssMigrationRulesDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	document, decodeErr := db2contract.DecodeCssMigrationRulesDocReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if !strings.Contains(document, "exemplar `aimee`") ||
		!strings.Contains(document, "Indexed rules in exemplar: **12**") ||
		!strings.Contains(document, "declarations: **5**") ||
		!strings.Contains(document, "BEM-like") {
		t.Fatalf("document = %q", document)
	}
	// One scan, so the three signals cannot disagree with each other.
	if len(store.sqlLog) != 1 {
		t.Errorf("statements = %d, want one scan", len(store.sqlLog))
	}
}

func TestRulesDocNamesTheConventionItFoundNoDelimitersFor(t *testing.T) {
	store := &fakeStore{row: cssSignalsRow(12, 0, 0)}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationRulesDocRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssMigrationRulesDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	document, decodeErr := db2contract.DecodeCssMigrationRulesDocReply(body)
	if decodeErr != nil || !strings.Contains(document, "flat / utility") {
		t.Fatalf("document = %q", document)
	}
	// A rule with forty declarations is still one rule.
	if !strings.Contains(store.lastSQL, "COUNT(DISTINCT c.id) AS rules") {
		t.Errorf("declarations inflate the rule count: %q", store.lastSQL)
	}
}

func TestRulesDocIsUngated(t *testing.T) {
	// The degraded path states what the scan found and asks a human to confirm
	// it. A document nobody has confirmed gates nothing, so the typed-fact flag
	// has nothing to protect here.
	installFlags(t, false, false)
	store := &fakeStore{row: cssSignalsRow(12, 5, 3)}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationRulesDocRequest("aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssMigrationRulesDoc), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if document, decodeErr := db2contract.DecodeCssMigrationRulesDocReply(body); decodeErr != nil ||
		!strings.Contains(document, "**12**") {
		t.Fatalf("document = %q (%v)", document, decodeErr)
	}
}

// assertConventions runs the operation against a scripted scan, with the two
// lock-and-assert row pairs the two facts need.
func assertConventions(t *testing.T, store *fakeStore, project string) (uint32,
	bus.ModuleStatus,
) {
	t.Helper()
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssMigrationAssertConventionsRequest(
		project, "2026-08-18T00:00:00Z")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssMigrationAssertConventions), request)
	if status != bus.ModuleStatusOK {
		return 0, status
	}
	asserted, decodeErr := db2contract.DecodeCssMigrationAssertConventionsReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	return asserted, status
}

func TestAssertConventionsCommitsWhatTheScanFound(t *testing.T) {
	installFlags(t, true, true)
	one := int64(1)
	store := &fakeStore{rowQueue: []*fakeRow{
		cssSignalsRow(12, 5, 3),
		{values: []any{one}}, {values: []any{one}},
		{values: []any{one}}, {values: []any{one}},
	}}
	asserted, status := assertConventions(t, store, "aimee")
	if status != bus.ModuleStatusOK || asserted != 2 {
		t.Fatalf("asserted = %d, status = %v", asserted, status)
	}
	// The delimiters were there and so were the custom properties, so both
	// findings are the positive ones.
	facts := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(facts, "INSERT INTO typed_facts") {
		t.Fatalf("nothing was asserted: %q", facts)
	}
	objects := []string{}
	for index, args := range store.argsLog {
		if strings.Contains(store.sqlLog[index], "INSERT INTO typed_facts") {
			objects = append(objects, args[3].(string))
		}
	}
	if len(objects) != 2 || objects[0] != "BEM" ||
		objects[1] != "css-custom-properties" {
		t.Fatalf("objects = %v", objects)
	}
	if store.txCalls != 2 {
		t.Errorf("transactions = %d, want one per fact", store.txCalls)
	}
}

func TestAssertConventionsNamesTheAbsences(t *testing.T) {
	installFlags(t, true, true)
	one := int64(1)
	store := &fakeStore{rowQueue: []*fakeRow{
		cssSignalsRow(12, 0, 0),
		{values: []any{one}}, {values: []any{one}},
		{values: []any{one}}, {values: []any{one}},
	}}
	if asserted, _ := assertConventions(t, store, "aimee"); asserted != 2 {
		t.Fatalf("asserted = %d", asserted)
	}
	objects := []string{}
	for index, args := range store.argsLog {
		if strings.Contains(store.sqlLog[index], "INSERT INTO typed_facts") {
			objects = append(objects, args[3].(string))
		}
	}
	if len(objects) != 2 || objects[0] != "flat-utility" ||
		objects[1] != "literal-values" {
		t.Fatalf("objects = %v", objects)
	}
}

func TestAssertConventionsAssertsNothingWithNothingIndexed(t *testing.T) {
	// "flat-utility" derived from zero rules is not a finding about the
	// project, it is a finding about the absence of a scan.
	installFlags(t, true, true)
	store := &fakeStore{row: cssSignalsRow(0, 0, 0)}
	asserted, status := assertConventions(t, store, "aimee")
	if status != bus.ModuleStatusOK || asserted != 0 {
		t.Fatalf("asserted = %d, status = %v", asserted, status)
	}
	if store.txCalls != 0 {
		t.Errorf("an unscanned project was asserted about")
	}
}

func TestAssertConventionsAssertsNothingWhenTheLayerIsOff(t *testing.T) {
	// The flag off means the rules document is the spec, so nothing is
	// committed as a fact -- and the scan is not even run, because its answer
	// could not be used.
	installFlags(t, true, false)
	store := &fakeStore{row: cssSignalsRow(12, 5, 3)}
	asserted, status := assertConventions(t, store, "aimee")
	if status != bus.ModuleStatusOK || asserted != 0 {
		t.Fatalf("asserted = %d, status = %v", asserted, status)
	}
	if len(store.sqlLog) != 0 {
		t.Errorf("a gated-off operation still read: %v", store.sqlLog)
	}
}

func TestTypedFactAssertHoldsTheSubjectAndRelation(t *testing.T) {
	// Two asserters of the same subject and relation would otherwise both find
	// no prior fact and both insert, leaving two active facts with no
	// supersession between them.
	installFlags(t, true, true)
	one := int64(1)
	store := &fakeStore{rowQueue: []*fakeRow{
		cssSignalsRow(12, 5, 3),
		{values: []any{one}}, {values: []any{one}},
		{values: []any{one}}, {values: []any{one}},
	}}
	if asserted, _ := assertConventions(t, store, "aimee"); asserted != 2 {
		t.Fatalf("asserted = %d", asserted)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "pg_advisory_xact_lock") {
		t.Errorf("the assert races itself again: %q", statements)
	}
	if !strings.Contains(statements, "FOR UPDATE") {
		t.Errorf("a prior fact could be superseded twice: %q", statements)
	}
	// The prior row is retained and marked, never deleted: a superseded fact is
	// how the layer answers what it used to believe.
	if !strings.Contains(statements, "SET active = 0, superseded_by") ||
		strings.Contains(statements, "DELETE FROM typed_facts") {
		t.Errorf("history is no longer retained: %q", statements)
	}
	// Re-asserting an unchanged object writes no row.
	if !strings.Contains(statements,
		"WHERE NOT EXISTS (SELECT 1 FROM prior WHERE prior.object = $4)") {
		t.Errorf("a settled convention would grow the table: %q", statements)
	}
}

func TestTypedFactAssertRollsBackAFailedInsert(t *testing.T) {
	installFlags(t, true, true)
	one := int64(1)
	store := &fakeStore{rowQueue: []*fakeRow{
		cssSignalsRow(12, 5, 3),
		{values: []any{one}}, {err: errTypedFactWrite},
		{values: []any{one}}, {values: []any{one}},
	}}
	asserted, status := assertConventions(t, store, "aimee")
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	// The second fact still lands: they are separate propositions and one
	// failing is not a reason to lose the other.
	if asserted != 1 {
		t.Fatalf("asserted = %d, want the one that succeeded", asserted)
	}
	if !store.rolledBack {
		t.Error("a failed assert was left half-applied")
	}
}

func TestSnapshotStoreRefusesAPhaseTheComparisonCannotFind(t *testing.T) {
	installFlags(t, true, true)
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssRenderSnapshotStoreRequest(
		"aimee", "src/app/button.css", "midway", `{"color":"red"}`, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCssRenderSnapshotStore), request); status !=
		bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want a refusal", status)
	}
	if len(store.sqlLog) != 0 {
		t.Error("a snapshot nothing will read was stored")
	}
}

func TestSnapshotStoreReplacesInOneStatement(t *testing.T) {
	// The C deletes and then inserts outside any transaction: between the two
	// the unit has no snapshot at all, and a failure between them leaves it
	// that way.
	installFlags(t, true, true)
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssRenderSnapshotStoreRequest(
		"aimee", "src/app/button.css", "before", `{"color":"red"}`, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssRenderSnapshotStore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr :=
		db2contract.DecodeCssRenderSnapshotStoreReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.execCalls != 1 ||
		strings.Contains(store.lastSQL, "DELETE FROM css_render_snapshots") {
		t.Errorf("the gap is back: %q", store.lastSQL)
	}
	// An undatable snapshot cannot be ordered against the conversion it is
	// evidence about.
	if !strings.Contains(store.lastSQL, "COALESCE(NULLIF($6, ''), pg_now_text())") {
		t.Errorf("a stampless snapshot stays undated: %q", store.lastSQL)
	}
	if store.lastArgs[4] != fnv1a64Hex(`{"color":"red"}`) {
		t.Errorf("content hash = %v", store.lastArgs[4])
	}
}

func TestSnapshotStoreStoresNothingWithTheStyleGraphOff(t *testing.T) {
	installFlags(t, false, true)
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssRenderSnapshotStoreRequest(
		"aimee", "src/app/button.css", "after", `{"color":"red"}`, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssRenderSnapshotStore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr :=
		db2contract.DecodeCssRenderSnapshotStoreReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want a plain no", acknowledged)
	}
	if len(store.sqlLog) != 0 {
		t.Error("a gated-off store still wrote")
	}
}

func TestSnapshotStoreSaysNoWhenTheProjectIsNotCurrent(t *testing.T) {
	// The insert selects from projects, so a project that is missing or
	// superseded matches nothing and the row is never written.
	installFlags(t, true, true)
	store := &fakeStore{execRowsAt: true}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssRenderSnapshotStoreRequest(
		"gone", "src/app/button.css", "after", "{}", "2026-08-18T00:00:00Z")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCssRenderSnapshotStore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if acknowledged, decodeErr :=
		db2contract.DecodeCssRenderSnapshotStoreReply(body); decodeErr != nil ||
		acknowledged != 0 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
}

func TestContentHashMatchesTheCsOwn(t *testing.T) {
	// The C's basis is one digit short of FNV-1a's, so these are not the
	// published FNV-1a values and must not be "corrected" to them: what a
	// content hash is for is telling a row the C wrote from a row this wrote,
	// and it can only do that if both spell it the same way.
	//
	// Nothing is lost by the shorter basis -- any odd basis mixes as well for
	// change detection -- and everything would be lost by changing it, because
	// every hash already stored would stop matching its own content.
	if fnv1a64Hex("") != "14650fb0739d0383" {
		t.Errorf("empty = %q", fnv1a64Hex(""))
	}
	if fnv1a64Hex("abc") != "e16801510db89efd" {
		t.Errorf("abc = %q", fnv1a64Hex("abc"))
	}
}

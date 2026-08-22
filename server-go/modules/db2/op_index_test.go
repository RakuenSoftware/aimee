package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEntityListActiveReadsCounts(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"postgres", int64(7)},
		{"pgvector", int64(3)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityListActiveRequest(3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityListActive), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeEntityListActiveReply(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(rows) != 2 || rows[0].EntityName != "postgres" || rows[0].ObservationCount != 7 {
		t.Fatalf("rows = %+v", rows)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != int64(3) {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestEntityListActiveStopsAtTheReplyCeiling(t *testing.T) {
	// The statement has no LIMIT -- the C caller stops at its own array -- so a
	// corpus with more entities than the reply holds is truncated here rather
	// than overflowing the encoder.
	values := make([][]any, db2contract.EntityListActiveMaxRows+5)
	for index := range values {
		values[index] = []any{"entity", int64(1)}
	}
	store := &fakeStore{rows: &fakeRows{values: values}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityListActiveRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityListActive), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeEntityListActiveReply(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(rows) != db2contract.EntityListActiveMaxRows {
		t.Fatalf("rows = %d, want the ceiling %d", len(rows),
			db2contract.EntityListActiveMaxRows)
	}
}

func TestEntityEdgeCoTargetsBindsBothHalvesOnce(t *testing.T) {
	// The C statement binds node, relation and weight twice, once per half of
	// the union. pgx numbers placeholders, so $1..$3 appear in both halves and
	// are bound once -- three fewer arguments to get out of step.
	store := &fakeStore{rows: &fakeRows{values: [][]any{{"replay-dst"}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeCoTargetsRequest("replay-src", "mentions", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgeCoTargets), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeEntityEdgeCoTargetsReply(body)
	if err != nil || len(rows) != 1 || rows[0].EdgeTarget != "replay-dst" {
		t.Fatalf("rows = %+v, err = %v", rows, err)
	}
	if len(store.lastArgs) != 4 {
		t.Fatalf("args = %v, want node, relation, weight and the limit", store.lastArgs)
	}
	if strings.Count(store.lastSQL, "$1") != 2 || strings.Count(store.lastSQL, "$4") != 1 {
		t.Error("the union's halves do not share their placeholders")
	}
	if !strings.Contains(store.lastSQL, "edge_class <> 'semantic'") {
		t.Error("semantic edges are not excluded")
	}
	if strings.Count(store.lastSQL, "cpg.state='visible'") != 2 {
		t.Error("the visibility projection is not applied to both halves")
	}
}

func TestEntityEdgeCoTargetsSkipsAnEmptyTarget(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{{""}, {"real"}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEntityEdgeCoTargetsRequest("node", "rel", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEntityEdgeCoTargets), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeEntityEdgeCoTargetsReply(body)
	if err != nil || len(rows) != 1 || rows[0].EdgeTarget != "real" {
		t.Fatalf("rows = %+v, err = %v", rows, err)
	}
}

func TestCodeIndexProjectListFiltersDottedRoots(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"replay-project", "replay/root", "2026-08-22T09:59:57Z"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeIndexProjectListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeIndexProjectList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeCodeIndexProjectListReply(body)
	if err != nil || len(rows) != 1 || rows[0].ProjectName != "replay-project" {
		t.Fatalf("rows = %+v, err = %v", rows, err)
	}
	// The filter is in the statement, not applied to the rows: a project rooted
	// under a dotted directory never arrives, and nothing in the answer says one
	// was withheld.
	if !strings.Contains(store.lastSQL, `root NOT LIKE '%/.%'`) {
		t.Error("the dotted-root filter is not in the statement")
	}
	if !strings.Contains(store.lastSQL, "lifecycle_state = 'current'") {
		t.Error("the list is not restricted to current generations")
	}
}

func TestEnrollmentListReadsTheRoster(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(1), "replay-scope", "abc", "01", "active", "2026-01-01", "2026-01-02",
			"2027-01-01", "", int32(1), "0123456789abcdef0123456789abcdef"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentListRequest(8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEnrollmentList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeEnrollmentListReply(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	// legacy comes before authority_id in the SELECT and after it in the reply,
	// so this pins that the scan order and the row construction stay apart.
	if len(rows) != 1 || rows[0].LegacyRow != 1 ||
		rows[0].AuthorityID != "0123456789abcdef0123456789abcdef" ||
		rows[0].CertSerialNorm != "01" {
		t.Fatalf("rows = %+v", rows)
	}
}

func TestEnrollmentListClampsTheLimitToTheReply(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentListRequest(
		db2contract.EnrollmentListMaxRows)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageEnrollmentList), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != db2contract.EnrollmentListMaxRows {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

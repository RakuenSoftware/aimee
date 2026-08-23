package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestContradictionsScopeBothSidesAgainstOneScope(t *testing.T) {
	// The reply exposes one key and both contents, so a conflict with one side
	// out of scope would leak that side's text to a caller who cannot see the
	// memory. Both sides are filtered, and both read the same four scope
	// placeholders -- filtering each against its own set would bind the limit
	// to the scope.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(10), int64(11), "2026-01-01T00:00:00Z",
			ptr("build-state"), ptr("the build is green"), ptr("the build is red")},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLifecycleUnresolvedContradictionsRequest(
		3, "live-probe-workspace", "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageLifecycleUnresolvedContradictions), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	conflicts, decodeErr :=
		db2contract.DecodeLifecycleUnresolvedContradictionsReply(body)
	if decodeErr != nil || len(conflicts) != 1 {
		t.Fatalf("conflicts = %+v", conflicts)
	}
	if conflicts[0].ConflictID != 4 || conflicts[0].AKey != "build-state" ||
		conflicts[0].AContent != "the build is green" ||
		conflicts[0].BContent != "the build is red" {
		t.Fatalf("conflict = %+v", conflicts[0])
	}
	if !strings.Contains(store.lastSQL, "asp.memory_id = ma.id") ||
		!strings.Contains(store.lastSQL, "asp.memory_id = mb.id") {
		t.Errorf("only one side is scoped: %q", store.lastSQL)
	}
	// Five arguments: the limit and the four scope values, bound once for both
	// sides.
	if len(store.lastArgs) != 5 {
		t.Fatalf("args = %v, want the limit and one scope", store.lastArgs)
	}
	if store.lastArgs[0] != int64(db2contract.LifecycleUnresolvedContradictionsMaxRows) {
		t.Errorf("limit = %v", store.lastArgs[0])
	}
	if store.lastArgs[3] != "live-probe-workspace" || store.lastArgs[4] != "aimee" {
		t.Errorf("scope = %v", store.lastArgs)
	}
}

func TestContradictionsLeadWithTheNearerSide(t *testing.T) {
	// A conflict touching the caller's own project should lead even when its
	// other side is merely shared, so the ordering takes the higher of the two
	// ranks rather than either one.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLifecycleUnresolvedContradictionsRequest(
		1, "", "aimee")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageLifecycleUnresolvedContradictions), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY CASE WHEN") ||
		!strings.Contains(store.lastSQL, "END DESC, c.detected_at DESC") {
		t.Errorf("the ordering changed: %q", store.lastSQL)
	}
	// A conflict whose memory has been deleted still has a row, and the scope
	// filter is what decides whether it survives -- an inner join would drop it
	// for a different reason.
	if !strings.Contains(store.lastSQL, "LEFT JOIN memories ma") ||
		!strings.Contains(store.lastSQL, "LEFT JOIN memories mb") {
		t.Errorf("the joins are no longer outer: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "WHERE c.resolved = 0") {
		t.Errorf("settled conflicts would be listed: %q", store.lastSQL)
	}
}

func TestContradictionsCarryScopeOnTheRequest(t *testing.T) {
	// The catalogue recorded this operation as unscoped while its statement was
	// scope-filtered, because the C wraps the macro in file-local aliases that
	// the generator's scan did not follow. Under an inactive scope the filter
	// admits everything, so porting it unscoped would have answered every
	// conflict in the database to a caller who asked for one project's.
	request, err := db2contract.EncodeLifecycleUnresolvedContradictionsRequest(
		3, "workspace", "project")
	if err != nil {
		t.Fatalf("the request no longer carries scope: %v", err)
	}
	flags, workspace, project, decodeErr :=
		db2contract.DecodeLifecycleUnresolvedContradictionsRequest(request)
	if decodeErr != nil || flags != 3 || workspace != "workspace" ||
		project != "project" {
		t.Fatalf("scope = %d %q %q", flags, workspace, project)
	}
}

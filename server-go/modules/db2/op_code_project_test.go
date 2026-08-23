package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func upsertProject(t *testing.T, store *fakeStore, project, root string) (uint64,
	bus.ModuleStatus,
) {
	t.Helper()
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeProjectUpsertRequest(project, root)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCodeProjectUpsert), request)
	if status != bus.ModuleStatusOK {
		return 0, status
	}
	projectID, decodeErr := db2contract.DecodeCodeProjectUpsertReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	return projectID, status
}

func TestProjectUpsertCreatesWhatIsNotThere(t *testing.T) {
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},
		{values: []any{int64(41)}},
	}}
	projectID, status := upsertProject(t, store, "aimee", "/srv/aimee")
	if status != bus.ModuleStatusOK || projectID != 41 {
		t.Fatalf("project id = %d, status = %v", projectID, status)
	}
	// The read locks the row it is about to decide from.
	if !strings.Contains(store.sqlLog[0], "FOR UPDATE") {
		t.Errorf("two upserts of one project could race: %q", store.sqlLog[0])
	}
	// One transaction: a project row pointing at a generation that does not
	// exist yet is worse than no row at all.
	if store.txCalls != 1 || !store.committed {
		t.Errorf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "INSERT INTO code_project_generations") {
		t.Errorf("no generation was recorded: %q", statements)
	}
	if !strings.Contains(statements, "INSERT INTO code_project_aliases") {
		t.Errorf("the checkout was not claimed: %q", statements)
	}
}

func TestProjectUpsertDoesNotAdvanceTheGenerationForAMove(t *testing.T) {
	// A checkout that moved changes the alias and the generation root. It is
	// not a new indexing generation: the contents did not change, and a new
	// generation would say they had.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), "/old/path", "current", int64(4)}},
		{err: pgx.ErrNoRows},
	}}
	if projectID, status := upsertProject(t, store, "aimee",
		"/new/path"); projectID != 7 || status != bus.ModuleStatusOK {
		t.Fatalf("project id = %d, status = %v", projectID, status)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if strings.Contains(statements, "SET state = 'superseded'") {
		t.Errorf("a move superseded the generation: %q", statements)
	}
	if !strings.Contains(statements, "SET is_current = 0") {
		t.Errorf("the old checkout still claims to be current: %q", statements)
	}
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "UPDATE projects") {
			if store.argsLog[index][1] != int64(4) {
				t.Errorf("generation = %v, want it unchanged",
					store.argsLog[index][1])
			}
		}
	}
}

func TestProjectUpsertAdvancesTheGenerationOnReattach(t *testing.T) {
	// Reattaching a project that was explicitly detached is the one event that
	// makes a new generation: what it had is superseded, not amended.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), "/same/path", "detached", int64(4)}},
		{err: pgx.ErrNoRows},
	}}
	if _, status := upsertProject(t, store, "aimee",
		"/same/path"); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "SET state = 'superseded'") {
		t.Errorf("the prior generation was left current: %q", statements)
	}
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "UPDATE projects") &&
			store.argsLog[index][1] != int64(5) {
			t.Errorf("generation = %v, want it advanced", store.argsLog[index][1])
		}
	}
}

func TestProjectUpsertClaimsOnlyRealCheckouts(t *testing.T) {
	// A label such as "remote" is deliberately not unique. Claiming it as an
	// alias would collide two projects that share a word.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(7), "remote", "current", int64(1)}},
	}}
	if _, status := upsertProject(t, store, "aimee",
		"remote"); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(strings.Join(store.sqlLog, "\n"),
		"code_project_aliases\n (project_id") {
		t.Error("a label was claimed as a checkout")
	}
}

func TestProjectUpsertTakesOverAClaimedCheckout(t *testing.T) {
	// A checkout another project already claims is a re-index under a new
	// name. Rejecting it made a directory scannable exactly once, ever.
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},
		{values: []any{int64(41)}},
		{values: []any{int64(9)}},
	}}
	projectID, status := upsertProject(t, store, "aimee-retry", "/srv/aimee")
	if status != bus.ModuleStatusOK || projectID != 41 {
		t.Fatalf("project id = %d, status = %v", projectID, status)
	}
	if !store.committed {
		t.Error("the takeover rolled the whole upsert back")
	}
}

func TestNodeKeysEncodeWhatAKeyCannotCarry(t *testing.T) {
	if key := fileNodeKey("aimee", "src/app/button.css"); key !=
		"file:aimee:src/app/button.css" {
		t.Errorf("file key = %q", key)
	}
	// A path separator stays readable; a space does not survive raw.
	if key := symbolNodeKey("my project", "Button Group"); key !=
		"symbol:my%20project:Button%20Group" {
		t.Errorf("symbol key = %q", key)
	}
	if key := projectNodeKey("aimee"); key != "project:aimee" {
		t.Errorf("project key = %q", key)
	}
}

func TestALongKeyBecomesAHashOfItself(t *testing.T) {
	long := strings.Repeat("a", graphEndpointMax)
	key := symbolNodeKey("aimee", long)
	if !strings.HasPrefix(key, "symbol:h:") || len(key) != len("symbol:h:")+32 {
		t.Fatalf("key = %q", key)
	}
	// Two keys differing only in prefix must not compact to the same thing.
	if key == buildNodeKey("file", "aimee:"+long) {
		t.Error("the prefix is not in the hash")
	}
}

func TestExportKeysAreCompactedRatherThanDropped(t *testing.T) {
	// The C snprintf-s these keys and skips the ones that overflow, so a
	// project with very long export names projects some of its exports and not
	// others, silently.
	long := strings.Repeat("e", graphEndpointMax)
	if key := prefixedNodeKey("export", "aimee", long); !strings.HasPrefix(key,
		"export:h:") {
		t.Fatalf("key = %q", key)
	}
}

func syncProject(t *testing.T, store *fakeStore) (uint64, bus.ModuleStatus) {
	t.Helper()
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionSyncProjectRequest("aimee", 900001)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageProjectionSyncProject), request)
	if status != bus.ModuleStatusOK {
		return 0, status
	}
	edgeCount, decodeErr := db2contract.DecodeProjectionSyncProjectReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	return edgeCount, status
}

// syncStore scripts one project, one file, and the six edge sources.
func syncStore(calls [][]any) *fakeStore {
	return &fakeStore{
		row: &fakeRow{values: []any{int64(3)}},
		rowsQueue: []*fakeRows{
			{values: [][]any{{int64(11), "src/app/button.css"}}},
			{values: [][]any{{int64(11), "renderButton", "function"}}},
			{values: [][]any{{int64(11), "Button"}}},
			{values: [][]any{{int64(11), "react"}}},
			{values: [][]any{{int64(11), "/buttons"}}},
			{values: [][]any{{int64(11), "card__title"}}},
			{values: calls},
		},
	}
}

func TestSyncProjectsEveryEdgeClass(t *testing.T) {
	store := syncStore([][]any{{int64(11), "renderButton", "paint"}})
	edgeCount, status := syncProject(t, store)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	// contains, defines, exports, imports, routes, styles, calls.
	if edgeCount != 7 {
		t.Fatalf("edges = %d", edgeCount)
	}
	// One transaction. The C has none, and half a projected project is
	// indistinguishable from a project that only had those edges.
	if store.txCalls != 1 || !store.committed {
		t.Errorf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "INSERT INTO entity_edges") ||
		!strings.Contains(statements, "INSERT INTO code_projection_edges") {
		t.Fatalf("statements = %q", statements)
	}
	// Two writes for the whole project, not two per edge.
	if store.execCalls != 3 {
		t.Errorf("write statements = %d, want the two batches and the counts",
			store.execCalls)
	}
	relations := edgeRelations(t, store)
	for _, wanted := range []string{"contains", "defines", "exports", "imports",
		"routes", "styles", "calls"} {
		if !relations[wanted] {
			t.Errorf("no %s edge was written: %v", wanted, relations)
		}
	}
}

func TestSyncPreservesWhatUseHasTaughtOnBothPaths(t *testing.T) {
	// Two paths, because the unique index the conflict clause names is built by
	// a migration rather than by the schema. Both have to carry the observed
	// weight and the accumulated utility across a re-projection: a projection
	// that unlearned them on an unmigrated instance would be a silent
	// difference between two deployments of the same code.
	//
	// The fake answers no rows to the index probe, so the fallback is what runs
	// here; the fast path is checked by reading its statement directly, because
	// the probe's answer is cached for the life of the process.
	store := syncStore(nil)
	if _, status := syncProject(t, store); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	statements := strings.Join(store.sqlLog, "\n")
	if !strings.Contains(statements, "COALESCE(r.weight, 0)") ||
		!strings.Contains(statements, "COALESCE(r.utility_score, 0)") ||
		!strings.Contains(statements, "COALESCE(r.utility_touched_at, '')") {
		t.Errorf("the fallback path unlearns what use taught: %q", statements)
	}
	for _, preserved := range []string{
		"weight = entity_edges.weight",
		"utility_score = entity_edges.utility_score",
		"utility_touched_at = entity_edges.utility_touched_at",
	} {
		if !strings.Contains(projectionEdgeUpsertQuery, preserved) {
			t.Errorf("the conflict path no longer preserves %s", preserved)
		}
	}
	// The stamp is the canonical one on both paths, not the session's timezone.
	if !strings.Contains(projectionEdgeUpsertQuery, "pg_now_text()") ||
		!strings.Contains(projectionEdgeReplaceQuery, "pg_now_text()") {
		t.Error("the structural stamp is no longer the canonical one")
	}
}

func TestSyncCountsEdgesRatherThanUpserts(t *testing.T) {
	// The C counts upserts, so two identical call rows in one file make a
	// generation claim two edges where the graph holds one.
	store := syncStore([][]any{
		{int64(11), "renderButton", "paint"},
		{int64(11), "renderButton", "paint"},
	})
	edgeCount, status := syncProject(t, store)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if edgeCount != 7 {
		t.Fatalf("edges = %d, want the duplicate collapsed", edgeCount)
	}
	// And the generation's own counts say the same thing.
	for index, sql := range store.sqlLog {
		if strings.Contains(sql, "UPDATE code_projection_generations") {
			if store.argsLog[index][1] != int64(7) ||
				store.argsLog[index][2] != int64(1) {
				t.Errorf("counts = %v", store.argsLog[index])
			}
		}
	}
}

func TestSyncRefusesAGenerationNobodyNamed(t *testing.T) {
	store := syncStore(nil)
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionSyncProjectRequest("aimee", 0)
	if err != nil {
		// The envelope may refuse it first, which is the same answer earlier.
		return
	}
	if _, status := handler(
		invocation(db2contract.StageProjectionSyncProject), request); status !=
		bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want a refusal", status)
	}
}

func TestSyncSaysNothingAboutAnUnindexedProject(t *testing.T) {
	store := &fakeStore{}
	if _, status := syncProject(t, store); status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want the C's refusal", status)
	}
}

// edgeRelations answers which relations the batch insert carried.
func edgeRelations(t *testing.T, store *fakeStore) map[string]bool {
	t.Helper()
	for index, sql := range store.sqlLog {
		if !strings.Contains(sql, "INSERT INTO entity_edges") {
			continue
		}
		relations, ok := store.argsLog[index][1].([]string)
		if !ok {
			t.Fatalf("relations = %#v", store.argsLog[index][1])
		}
		found := map[string]bool{}
		for _, relation := range relations {
			found[relation] = true
		}
		return found
	}
	t.Fatal("no edges were written")
	return nil
}

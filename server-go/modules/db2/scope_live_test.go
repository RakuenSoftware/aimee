package db2

import (
	"context"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// Four memories with the same kind and tier, differing only in how they are
// scoped. They all satisfy global_constraints' own predicates, so anything that
// separates them is the scope filter and nothing else.
const (
	scopeProbeWorkspace = "scope-probe-workspace"
	scopeProbeProject   = "scope-probe-project"
	scopeProbeSeed      = `
INSERT INTO memories (id, tier, kind, key, content, confidence, created_at, updated_at) VALUES
 (900101, 'L2', 'policy', 'scope-probe-mine', 'in my workspace', 0.9,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00'),
 (900102, 'L2', 'policy', 'scope-probe-theirs', 'in another workspace', 0.9,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00'),
 (900103, 'L2', 'policy', 'scope-probe-shared', 'shared with everyone', 0.9,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00'),
 (900104, 'L2', 'policy', 'scope-probe-untagged', 'written before scoping', 0.9,
  '2026-01-01 00:00:00', '2026-01-01 00:00:00');
INSERT INTO memory_scopes (memory_id, scope_type, scope_value) VALUES
 (900101, 'workspace', 'scope-probe-workspace'),
 (900102, 'workspace', 'scope-probe-other-workspace'),
 (900103, 'global', '_global');`
)

// TestLiveScopeActuallyFiltersOtherWorkspaces is the test the whole scope change
// exists for.
//
// Every other scope test asserts on the statement text or on a fake. This one
// puts four differently-scoped memories in a real database and reads them back
// through the dispatcher, because the defect being closed was not a statement
// that looked wrong -- it was a correct statement executed with nothing bound,
// where the filter silently admits every row.
func TestLiveScopeActuallyFiltersOtherWorkspaces(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	direct, ok := store.(*PoolStore)
	if !ok {
		// Seeds fixtures through the pool directly, which the storage wire
		// does not expose. Widening the wire so a test can reach past it would
		// be widening it for something no operation needs.
		t.Skip("this test uses the pool directly; run it without AIMEE_DB2_STORE=bus")
	}
	pool := direct.pool

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, scopeProbeSeed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	handler := NewDispatchHandler(&rollbackStore{tx: tx})
	read := func(t *testing.T, flags uint32) map[string]bool {
		t.Helper()
		request, err := db2contract.EncodeGlobalConstraintsRequest(
			flags, scopeProbeWorkspace, scopeProbeProject)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		body, status := handler(invocation(db2contract.StageGlobalConstraints), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		rows, err := db2contract.DecodeGlobalConstraintsReply(body)
		if err != nil {
			t.Fatalf("decode reply: %v", err)
		}
		keys := map[string]bool{}
		for _, row := range rows {
			keys[row.MemoryKey] = true
		}
		return keys
	}

	t.Run("an active scope excludes another workspace", func(t *testing.T) {
		keys := read(t, 1)
		if !keys["scope-probe-mine"] {
			t.Error("a memory scoped to the caller's workspace was not returned")
		}
		if keys["scope-probe-theirs"] {
			t.Error("another workspace's memory was returned; this is the defect")
		}
		// Shared and untagged rows stay visible: the rank treats an untagged
		// memory as shared, which is what keeps rows written before scoping
		// existed readable.
		if !keys["scope-probe-shared"] || !keys["scope-probe-untagged"] {
			t.Error("a shared or untagged memory was hidden by the scope")
		}
	})

	t.Run("include-all admits the other workspace", func(t *testing.T) {
		keys := read(t, 3)
		if !keys["scope-probe-theirs"] {
			t.Error("include-all did not admit the other workspace's memory")
		}
	})

	t.Run("an inactive scope admits everything", func(t *testing.T) {
		// Not an endorsement -- this is exactly the behaviour that made an
		// unscoped call dangerous. It is pinned so that the danger stays
		// visible: the fix is that the scope now travels, not that inactive
		// became safe.
		keys := read(t, 0)
		if !keys["scope-probe-theirs"] {
			t.Error("an inactive scope no longer admits every row; if this is " +
				"deliberate the operations no longer need to carry the scope")
		}
	})
}

// TestLiveScopeSurvivesEveryScopedOperation runs all sixteen against the seeded
// rows, so a statement that binds the scope in the wrong position fails here
// rather than silently ranking by the caller's own arguments.
func TestLiveScopeSurvivesEveryScopedOperation(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	direct, ok := store.(*PoolStore)
	if !ok {
		// Seeds fixtures through the pool directly, which the storage wire
		// does not expose. Widening the wire so a test can reach past it would
		// be widening it for something no operation needs.
		t.Skip("this test uses the pool directly; run it without AIMEE_DB2_STORE=bus")
	}
	pool := direct.pool

	for _, testCase := range scopedOperations() {
		t.Run(testCase.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			tx, err := pool.Begin(ctx)
			if err != nil {
				t.Fatalf("begin: %v", err)
			}
			defer func() { _ = tx.Rollback(ctx) }()
			if _, err := tx.Exec(ctx, scopeProbeSeed); err != nil {
				t.Fatalf("seed: %v", err)
			}
			handler := NewDispatchHandler(&rollbackStore{tx: tx})
			request, err := testCase.build(1, scopeProbeWorkspace, scopeProbeProject)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(testCase.stage), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v -- the scoped statement did not run", status)
			}
		})
	}
}

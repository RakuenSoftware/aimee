package db2

import (
	"context"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// Two pending commitments, seeded so that the two orderings disagree.
//
// The undated one is created LATER than the dated one's deadline. With the
// fallback reachable it stands in its creation date and sorts second; with the
// fallback dead its ttl_at is the empty string, which sorts before every real
// date, and it sorts first. Seeding it earlier would put it first either way
// and prove nothing -- which is exactly what the first version of this test
// did.
const openCommitmentSeed = `
INSERT INTO memories
 (id, tier, kind, key, content, lifecycle_state, ttl_at, created_at, updated_at)
VALUES
 (900301, 'L2', 'task', 'commitment-due', 'due in february', 'pending',
  '2026-02-01 00:00:00', '2026-01-01 00:00:00', '2026-01-01 00:00:00'),
 (900302, 'L2', 'task', 'commitment-undated', 'no deadline', 'pending',
  '', '2026-09-01 00:00:00', '2026-09-01 00:00:00');`

// TestLiveOpenCommitmentsPutDatedWorkFirst proves the ordering, which a fake
// cannot: the fault was in an ORDER BY expression, and a fake returns rows in
// whatever order it was handed them.
//
// memories.ttl_at is NOT NULL with an empty-string default, so
// COALESCE(m.ttl_at, m.created_at) always returned ttl_at and never reached the
// fallback. An empty string sorts before every real date, so a commitment with
// no deadline was surfaced ahead of one that was actually due.
func TestLiveOpenCommitmentsPutDatedWorkFirst(t *testing.T) {
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
	if _, err := tx.Exec(ctx, openCommitmentSeed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	handler := NewDispatchHandler(&rollbackStore{tx: tx})
	request, err := db2contract.EncodeRecallSectionRequest(
		recallSectionOpenCommitments, 1, "live-probe-workspace", "live-probe-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRecallSection), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, decodeErr := db2contract.DecodeRecallSectionReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}

	var order []string
	for _, row := range rows {
		if row.MemoryKey == "commitment-due" || row.MemoryKey == "commitment-undated" {
			order = append(order, row.MemoryKey)
		}
	}
	if len(order) != 2 {
		t.Fatalf("seeded commitments returned = %v, want both", order)
	}
	if order[0] != "commitment-due" {
		t.Fatalf("order = %v; the undated commitment was surfaced ahead of the "+
			"one that is due, so the deadline fallback is unreachable again", order)
	}
}

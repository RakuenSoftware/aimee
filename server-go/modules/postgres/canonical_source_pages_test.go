package postgres

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	wirefields "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Use the actual SQL encoder/decoder and reject the same oversized body that
// the real bus rejects. No process execution or test verdict is manufactured.
type sourcePageCaller struct {
	provider *sqlHandler
	max      int
	calls    int
}

func (c *sourcePageCaller) Call(ctx context.Context, kind, stage uint32, trace uint64, timeout time.Duration, request []byte) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if kind != db.EventPostgresSQL || stage != db.StagePostgresSQL {
		return nil, fmt.Errorf("unexpected SQL route")
	}
	body, status := c.provider.handleContext(ctx, bus.ModuleInvocation{StageID: stage}, request)
	if status != bus.ModuleStatusOK {
		return nil, fmt.Errorf("SQL transport status %d", status)
	}
	var err error
	if len(body) > c.max {
		c.max = len(body)
	}
	c.calls++
	if len(body) > int(bus.ModuleMessageMaxBody) {
		return nil, fmt.Errorf("SQL reply exceeds real module body limit: %d", len(body))
	}
	return body, err
}
func sourcePageStore(t *testing.T, pool *pgxpool.Pool) (db.Store, *sourcePageCaller) {
	t.Helper()
	h := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	c := &sourcePageCaller{provider: h}
	s, err := db.NewStore(c)
	if err != nil {
		t.Fatal(err)
	}
	return s, c
}
func sourcePageSchema(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql", "schema_learning_projection_outbox.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql", "schema_telemetry.sql", "schema_delegation.sql"} {
		raw, err := os.ReadFile(filepath.Join("..", "aimee", "families", name))
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(context.Background(), string(raw)); err != nil {
			t.Fatal(name, err)
		}
	}
}
func sourcePageRun(t *testing.T, s db.Store, family store.Family, op uint32, fields ...string) []string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tx, err := s.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	status, cells, err := family.Ops[op].Run(ctx, tx, fields)
	if err != nil || status != store.StatusOK {
		t.Fatalf("op%d status%d: %v", op, status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	return cells
}
func sourcePageJSON(v any) string { raw, _ := json.Marshal(v); return string(raw) }

func TestCanonicalNativeInterruptionSourcePagesSQLWire(t *testing.T) {
	pool := sharedTestDatabase(t)
	sourcePageSchema(t, pool)
	s, wire := sourcePageStore(t, pool)
	ctx := context.Background()
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('session','actor')`); err != nil {
		t.Fatal(err)
	}
	run := func(op uint32, f ...string) []string { return sourcePageRun(t, s, families.Sessions, op, f...) }
	run(28, "session", "running", "turn", "actor", `{"kind":"execution","payload":{"status":"running"}}`)
	descriptor := `{"version":1,"kind":"metadata-fixture-not-execution-proof"}`
	sum := sha256.Sum256([]byte(descriptor))
	binding, owner, containment := strings.Repeat("b", 64), strings.Repeat("a", 32), hex.EncodeToString(sum[:])
	reservation := map[string]any{"version": 1, "session_id": "session", "turn_id": "turn", "harness": "codex", "binding_sha256": binding, "owner_id": owner, "containment_sha256": containment, "containment_json": descriptor, "epoch": "0"}
	run(40, "actor", sourcePageJSON(reservation))
	claim := map[string]any{"version": 1, "session_id": "session", "turn_id": "turn", "harness": "codex", "binding_sha256": binding, "owner_id": owner, "containment_sha256": containment, "epoch": "0", "scope_sha256": strings.Repeat("c", 64), "context_sha256": strings.Repeat("d", 64), "claim_id": strings.Repeat("e", 32), "expected_token": ""}
	run(38, "actor", sourcePageJSON(claim))
	nativeOwner := map[string]any{"version": 1, "harness": "codex", "owner_id": owner, "binding_sha256": binding, "epoch": "0", "containment_sha256": containment}
	for i := 0; i < 16; i++ {
		event := map[string]any{"kind": "message", "message_id": fmt.Sprint("message-", i), "native_owner": nativeOwner, "payload": map[string]any{"role": "assistant", "blocks": []any{map[string]any{"type": "text", "text": ""}, map[string]any{"type": "tool_use", "tool_id": fmt.Sprint("call-", i), "name": "read_file", "input": map[string]any{}}}}}
		raw := sourcePageJSON(event)
		raw = strings.Replace(raw, `"text":""`, `"text":"`+strings.Repeat("x", (1<<20)-256-len(raw))+`"`, 1)
		run(47, "session", fmt.Sprint(i)+strings.Repeat("i", 1000), "turn", "actor", raw)
	}
	// The old full-source query exceeds the actual transport frame although its
	// raw payload total is valid. This proves paging is required before reduction.
	rows, err := s.Query(ctx, `SELECT sequence::text,event_id,actor,payload FROM session_ir_events WHERE session_id='session' AND payload::jsonb->>'kind'='message' ORDER BY session_ir_events.sequence`)
	if err == nil {
		rows.Close()
		t.Fatal("unpaged source unexpectedly fit wire limit")
	}
	if wire.max <= int(bus.ModuleMessageMaxBody) {
		t.Fatal("negative probe did not reach real frame limit", wire.max, err)
	}
	wire.max = 0
	wire.calls = 0
	delete(reservation, "containment_json")
	reservation["expected_token"] = "pending:" + strings.Repeat("e", 32) + ":turn"
	frame, err := wirefields.EncodeFields(41, []string{"actor", sourcePageJSON(reservation)})
	if err != nil {
		t.Fatal(err)
	}
	// Exercise the real family dispatcher, including its owned transaction and
	// deadline. A two-second ordinary operation budget cannot scan this valid
	// near-capacity source; cancellation must leave no partial receipts.
	ordinary := families.Sessions
	ordinary.Ops = make(map[uint32]store.Op)
	for id, spec := range families.Sessions.Ops {
		ordinary.Ops[id] = spec
	}
	spec := ordinary.Ops[41]
	spec.Timeout = 2 * time.Second
	ordinary.Ops[41] = spec
	rejected, transport := ordinary.Handler(s)(bus.ModuleInvocation{StageID: ordinary.Stage}, frame)
	if transport != bus.ModuleStatusInternal || len(rejected) != 0 {
		t.Fatal("ordinary budget did not report storage timeout", transport)
	}
	var partial int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE payload::jsonb->>'kind'='tool_interruption'`).Scan(&partial); err != nil || partial != 0 {
		t.Fatal("cancelled scan persisted partial receipts", partial, err)
	}
	started := time.Now()
	reply, transport := families.Sessions.Handler(s)(bus.ModuleInvocation{StageID: families.Sessions.Stage}, frame)
	status, _, decodeErr := wirefields.DecodeFields(reply)
	if transport != bus.ModuleStatusOK || decodeErr != nil || status != store.StatusOK {
		t.Fatal("bounded history dispatcher refused valid source", transport, status, decodeErr)
	}
	t.Logf("actual paged native reconciliation %s, largest SQL frame %d", time.Since(started), wire.max)
	var count int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE payload::jsonb->>'kind'='tool_interruption'`).Scan(&count); err != nil || count != 16 {
		t.Fatal("complete source receipts", count, err)
	}
	if wire.max > 9<<20 {
		t.Fatal("page exceeded conservative frame budget", wire.max)
	}
}

func TestCanonicalDelegateExperienceSourcePagesSQLWire(t *testing.T) {
	pool := sharedTestDatabase(t)
	sourcePageSchema(t, pool)
	s, wire := sourcePageStore(t, pool)
	ctx := context.Background()
	sum := sha256.Sum256([]byte("parent:1:durable"))
	child := "delegate-" + hex.EncodeToString(sum[:])
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('parent','actor'),($1,'actor');`, child); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO agent_jobs(id,role,agent_name,status) VALUES(1,'engineer','fixture','running')`); err != nil {
		t.Fatal(err)
	}
	binding := map[string]any{"version": 1, "job_id": "1", "scope_kind": "project", "scope_id": "project", "agent": "fixture", "provider": "provider", "model": "model"}
	parent := sourcePageJSON(map[string]any{"kind": "child_execution", "payload": map[string]any{"session_id": child, "turn_id": "turn", "role": "engineer", "status": "running", "learning_binding": binding}})
	// Match the actual protected parent producer and ordinary child source API.
	cells := sourcePageRun(t, s, families.Sessions, 36, "parent", "bound-parent", "turn", "actor", parent)
	source := sourcePageJSON(map[string]any{"kind": "canonical_session_log", "session_id": "parent", "turn_id": "turn", "sequence": cells[0], "event_id": "bound-parent", "actor": "actor"})
	for i := 1; i <= 70; i++ {
		role := "user"
		blocks := []any{map[string]any{"type": "text", "text": "small"}}
		if i <= 20 {
			blocks = []any{map[string]any{"type": "text", "text": strings.Repeat("x", 950000)}}
		}
		if i == 9 || i == 10 || i == 65 || i == 66 {
			role = "assistant"
			blocks = []any{map[string]any{"type": "tool_use", "tool_id": fmt.Sprint("tool-", i), "name": "read_file", "input": map[string]any{}}}
		}
		raw := sourcePageJSON(map[string]any{"kind": "message", "message_id": fmt.Sprint("message-", i), "payload": map[string]any{"role": role, "blocks": blocks}})
		sourcePageRun(t, s, families.Sessions, 28, child, fmt.Sprint("event-", i), "turn", "actor", raw)
	}
	sourcePageRun(t, s, families.Sessions, 28, child, "terminal", "turn", "actor", `{"kind":"execution","payload":{"status":"completed"}}`)
	rows, err := s.Query(ctx, `SELECT sequence::text,event_id,payload FROM session_ir_events WHERE session_id=$1 AND payload::jsonb->>'kind'='message' ORDER BY session_ir_events.sequence LIMIT 65`, child)
	if err == nil {
		rows.Close()
		t.Fatal("unpaged candidate source unexpectedly fit wire limit")
	}
	if wire.max <= int(bus.ModuleMessageMaxBody) {
		t.Fatal("negative probe did not reach real frame limit", wire.max, err)
	}
	wire.max = 0
	wire.calls = 0
	frame, err := wirefields.EncodeFields(63, []string{"actor", source})
	if err != nil {
		t.Fatal(err)
	}
	ordinary := families.Telemetry
	ordinary.Ops = make(map[uint32]store.Op)
	for id, spec := range families.Telemetry.Ops {
		ordinary.Ops[id] = spec
	}
	spec := ordinary.Ops[63]
	spec.Timeout = 2 * time.Second
	ordinary.Ops[63] = spec
	rejected, transport := ordinary.Handler(s)(bus.ModuleInvocation{StageID: ordinary.Stage}, frame)
	if transport != bus.ModuleStatusInternal || len(rejected) != 0 {
		t.Fatal("ordinary delegate budget did not report storage timeout", transport)
	}
	var partial int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='delegate_experience'`).Scan(&partial); err != nil || partial != 0 {
		t.Fatal("cancelled scan wrote experience", partial, err)
	}
	started := time.Now()
	reply, transport := families.Telemetry.Handler(s)(bus.ModuleInvocation{StageID: families.Telemetry.Stage}, frame)
	status, _, decodeErr := wirefields.DecodeFields(reply)
	if transport != bus.ModuleStatusOK || decodeErr != nil || status != store.StatusOK {
		t.Fatal("bounded delegate dispatcher refused valid source", transport, status, decodeErr)
	}
	t.Logf("actual paged delegate source projection %s, largest SQL frame %d", time.Since(started), wire.max)
	var raw string
	if err := pool.QueryRow(ctx, `SELECT payload FROM interaction_events WHERE event_type='delegate_experience'`).Scan(&raw); err != nil {
		t.Fatal(err)
	}
	var experience struct {
		Qualification string `json:"qualification"`
		Actions       []struct {
			Tool     string `json:"tool_id"`
			Sequence string `json:"sequence"`
		} `json:"actions"`
	}
	if err := json.Unmarshal([]byte(raw), &experience); err != nil {
		t.Fatal(err)
	}
	if experience.Qualification != "ungraded" || len(experience.Actions) != 3 || experience.Actions[0].Tool != "tool-9" || experience.Actions[1].Tool != "tool-10" || experience.Actions[2].Tool != "tool-65" {
		t.Fatalf("first65 candidate/numeric source semantics lost: %+v", experience)
	}
	if wire.max > 9<<20 {
		t.Fatal("page exceeded frame budget", wire.max)
	}
	sourcePageRun(t, s, families.Telemetry, 63, "actor", source)
	var count int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='delegate_experience'`).Scan(&count); err != nil || count != 1 {
		t.Fatal("replay idempotence", count, err)
	}
}

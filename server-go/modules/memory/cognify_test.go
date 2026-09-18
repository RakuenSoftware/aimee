package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestCognifyParser(t *testing.T) {
	for _, raw := range []string{"", "null", "[]", "{", `{} {}`, strings.Repeat("x", episodeMaxOutput+1)} {
		if _, err := parseCognify([]byte(raw)); err == nil {
			t.Fatalf("accepted malformed/big response %.20q", raw)
		}
	}
	if out, err := parseCognify([]byte(`{"summary":"A quiet session."}`)); err != nil || out.Summary != "A quiet session." || len(out.Claims) != 0 {
		t.Fatal(out, err)
	}

	long := strings.Repeat("界", 4000)
	raw, _ := json.Marshal(map[string]any{"summary": long, "memory_kind": "episodic", "relations": []any{nil, 1, map[string]any{"subject": "Alice", "relation": "knows", "object": "Bob", "fact_text": long}}, "claims": []any{map[string]any{"subject": "a", "attribute": 1, "value": "bad"}, map[string]any{"subject": "a", "attribute": "b", "value": long}}, "coref_bindings": []any{map[string]any{"entity": ""}, map[string]any{"entity": "Alice"}}})
	out, err := parseCognify(raw)
	if err != nil || out.Summary != long || len(out.Relations) != 1 || out.Relations[0].FactText != long || len(out.Claims) != 1 || out.Claims[0].Kind != "fact" || out.Claims[0].Value != long || len(out.Coref) != 1 || out.Coref[0].Confidence != 0.5 {
		t.Fatal(out, err)
	}
	list := []any{}
	for i := 0; i < 30; i++ {
		list = append(list, map[string]any{"subject": "a", "relation": "r", "object": "b", "attribute": "c", "value": "v", "entity": "a"})
	}
	raw, _ = json.Marshal(map[string]any{"relations": list, "claims": list, "coref_bindings": list})
	out, err = parseCognify(raw)
	if err != nil || len(out.Relations) != 16 || len(out.Claims) != 16 || len(out.Coref) != 8 {
		t.Fatal(out, err)
	}
}

func exerciseCognifyReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	s := *backend
	enabled, async := true, false
	s.settings = func() (map[string]any, error) {
		return map[string]any{"memory_cognify_enabled": enabled, "memory_cognify_command": "fixture", "memory_cognify_async_enabled": async}, nil
	}
	long := strings.Repeat("🦊", 3000)
	var source, hidden, global int64
	for _, entry := range []struct {
		id                  *int64
		key, scope, project string
	}{{&source, "cognify-source", "project", "cognify-visible"}, {&hidden, "cognify-hidden", "project", "cognify-private"}, {&global, "cognify-global", "global", "_global"}} {
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact',$1,$2,$3,$4) RETURNING id`, entry.key, long, entry.scope, entry.project).Scan(entry.id); err != nil {
			t.Fatal(err)
		}
	}
	calls := 0
	fail := false
	var reply any = map[string]any{"summary": long, "memory_kind": "procedural", "relations": []any{map[string]any{"subject": "cognify-alice", "relation": "uses", "object": "Go", "fact_text": long}}, "claims": []any{map[string]any{"subject": "cognify-alice", "attribute": "editor", "value": long, "kind": "preference"}, map[string]any{"subject": "cognify-alice", "attribute": "hunch", "value": "possibly", "kind": "opinion"}, map[string]any{"subject": "cognify-alice", "attribute": "credential", "value": "password=hunter2"}}}
	s.episodeCommand = func(ctx context.Context, command string, input []byte) ([]byte, error) {
		calls++
		var parsed struct {
			UnitID int64  `json:"unit_id"`
			Text   string `json:"text"`
		}
		if json.Unmarshal(input, &parsed) != nil || parsed.UnitID <= 0 || parsed.Text != long {
			t.Fatalf("bad source payload %.200s", input)
		}
		if _, ok := ctx.Deadline(); !ok {
			t.Fatal("unbounded model execution")
		}
		if fail {
			return nil, errors.New("secret model stderr")
		}
		return json.Marshal(reply)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &s))
	call := func(verb string, id int64, project string) map[string]any {
		t.Helper()
		args := fmt.Sprintf(`{"unit":%d,"scope_context":true,"project":%q}`, id, project)
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, verb, args)
		t.Logf("cognify call %s %d %s: %v %v", verb, id, project, result["status"], result["kind"])
		if status != bus.ModuleStatusOK {
			t.Fatal(result, status)
		}
		return result
	}
	if r := call("cognify", hidden, "cognify-visible"); r["kind"] != "not_found" || calls != 0 {
		t.Fatal(r, calls)
	}
	r := call("cognify", source, "cognify-visible")
	if r["status"] != "ok" || r["summary"] != long || calls != 1 {
		t.Fatal(r, calls)
	}
	scalar := func(sql string, args ...any) int {
		t.Helper()
		var n int
		if err := tx.QueryRow(ctx, sql, args...).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE source_session='cognify' AND scope_type='project' AND scope_value='cognify-visible' AND confidence<=0.8`); n != 3 {
		t.Fatal("claims missing", n)
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE key='cognify-alice:hunch' AND confidence=0.5`); n != 1 {
		t.Fatal("opinion confidence", n)
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE key='cognify-alice:credential' AND content='[REDACTED]'`); n != 1 {
		t.Fatal("claim gate", n)
	}
	if n := scalar(`SELECT count(*) FROM rules WHERE title LIKE 'cognify-alice:%'`); n != 0 {
		t.Fatal("scoped content disclosed through global rules", n)
	}
	if n := scalar(`SELECT count(*) FROM memory_fact_actors a JOIN memories m ON m.id=a.memory_id WHERE m.source_session='cognify' AND a.actor_role='model' AND a.authority_rank=10 AND a.authenticated=0`); n != 3 {
		t.Fatal("claim actor", n)
	}
	if n := scalar(`SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1 AND status='pending'`, source); n != 1 {
		t.Fatal("kind not indexed", n)
	}
	// A replay retains relation and claim identity, and does not duplicate lineage.
	if r := call("cognify", source, "cognify-visible"); r["status"] != "ok" {
		t.Fatal(r)
	}
	if n := scalar(`SELECT count(*) FROM memory_relations WHERE memory_id=$1`, source); n != 1 {
		t.Fatal("duplicate relation", n)
	}
	if n := scalar(`SELECT count(*) FROM memory_lineage WHERE source_kind='memory' AND source_ref=$1`, fmt.Sprintf("memory:%d", source)); n != 3 {
		t.Fatal("duplicate lineage", n)
	}
	if r := call("cognify", global, "cognify-visible"); r["status"] != "ok" {
		t.Fatal(r)
	}
	if n := scalar(`SELECT count(*) FROM rules WHERE title='cognify-alice:editor'`); n != 1 {
		t.Fatal("global behavioral rule missing", n)
	}
	// Public counts cannot reveal another project's queue.
	async = true
	before := calls
	for _, entry := range []struct {
		id      int64
		project string
	}{{source, "cognify-visible"}, {hidden, "cognify-private"}} {
		if r := call("cognify", entry.id, entry.project); r["queued"] != true {
			t.Fatal(r)
		}
	}
	if calls != before {
		t.Fatal("async ran model")
	}
	if r := call("cognify_status", 0, "cognify-visible"); r["pending"] != float64(1) || r["total"] != float64(1) {
		t.Fatal(r)
	}
	fail = true
	if r := call("cognify_drain", 0, "cognify-visible"); r["processed"] != float64(1) || r["retried"] != float64(1) || r["failed"] != float64(1) {
		t.Fatal(r)
	}
	if n := scalar(`SELECT count(*) FROM kb_async_jobs WHERE kind='memory_cognify' AND document_id=$1 AND last_error='cognification attempt failed' AND attempts=1`, source); n != 1 {
		t.Fatal("retry status", n)
	}
	if _, err := tx.Exec(ctx, `UPDATE kb_async_jobs SET next_attempt_at='' WHERE kind='memory_cognify' AND document_id=$1`, source); err != nil {
		t.Fatal(err)
	}
	fail = false
	if r := call("cognify_drain", 0, "cognify-visible"); r["processed"] != float64(1) || r["done"] != float64(1) {
		t.Fatal(r)
	}
	if calls != before+2 {
		t.Fatal("worker re-enqueued instead of executing", calls, before)
	}
	enabled = false
	if r := call("cognify", source, "cognify-visible"); r["kind"] != "disabled" {
		t.Fatal(r)
	}
	enabled = true
	async = false
	// Fail after relation insertion: tombstones must roll back all derived writes.
	if _, err := tx.Exec(ctx, `INSERT INTO memory_rejection_tombstones(object_kind,memory_key,memory_content,scope_type,scope_value)
 VALUES('memory','cognify-refused:value','no','project','cognify-visible')`); err != nil {
		t.Fatal(err)
	}
	reply = map[string]any{"memory_kind": "episodic", "relations": []any{map[string]any{"subject": "rollback", "relation": "uses", "object": "nothing"}}, "claims": []any{map[string]any{"subject": "cognify-refused", "attribute": "value", "value": "no"}}}
	if r := call("cognify", source, "cognify-visible"); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	if n := scalar(`SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND src_entity='rollback'`, source); n != 0 {
		t.Fatal("partial cognification committed", n)
	}
	if n := scalar(`SELECT count(*) FROM memories WHERE id=$1 AND cognified_memory_kind='procedural'`, source); n != 1 {
		t.Fatal("kind partially committed", n)
	}

	// A plain semantic fact must not be promoted into a behavioral rule.
	reply = map[string]any{"memory_kind": "semantic", "claims": []any{map[string]any{"subject": "cognify-server", "attribute": "port", "value": "5432"}}}
	if r := call("cognify", global, "cognify-visible"); r["status"] != "ok" {
		t.Fatal(r)
	}
	if n := scalar(`SELECT count(*) FROM rules WHERE title='cognify-server:port'`); n != 0 {
		t.Fatal("ordinary fact became rule", n)
	}
	// Duplicate enqueue is one durable job, and an explicit request can reset a
	// terminal failure. Automatic retries stop after three attempts.
	async = true
	fail = true
	if r := call("cognify", source, "cognify-visible"); r["queued"] != true {
		t.Fatal(r)
	}
	if r := call("cognify", source, "cognify-visible"); r["queued"] != true {
		t.Fatal(r)
	}
	for i := 1; i <= 3; i++ {
		if _, err := tx.Exec(ctx, `UPDATE kb_async_jobs SET next_attempt_at='' WHERE kind='memory_cognify' AND document_id=$1`, source); err != nil {
			t.Fatal(err)
		}
		r := call("cognify_drain", 0, "cognify-visible")
		want := float64(1)
		if i == 3 {
			want = 0
		}
		if r["processed"] != float64(1) || r["retried"] != want || r["total"] != float64(1) {
			t.Fatal(r)
		}
	}
	if r := call("cognify_drain", 0, "cognify-visible"); r["processed"] != float64(0) {
		t.Fatal(r)
	}
	enabled = false
	if r := call("cognify_drain", 0, "cognify-visible"); r["status"] != "disabled" {
		t.Fatal(r)
	}
	enabled = true
	async = false
	fail = false
	// Retired or malformed ancestry must prevent even sending text to the model.
	before = calls
	if _, err := tx.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref) VALUES('memory',$1,'memory','memory:bad')`, source); err != nil {
		t.Fatal(err)
	}
	if r := call("cognify", source, "cognify-visible"); r["kind"] != "unavailable" || calls != before {
		t.Fatal(r, calls)
	}
	if _, err := tx.Exec(ctx, `UPDATE memory_lineage SET source_ref=$2 WHERE object_type='memory' AND object_id=$1 AND source_ref='memory:bad'`, source, fmt.Sprintf("memory:%d", source)); err != nil {
		t.Fatal(err)
	}
	if r := call("cognify", source, "cognify-visible"); r["kind"] != "unavailable" || calls != before {
		t.Fatal(r, calls)
	}
}

func TestCognifyConfigurationFailsClosed(t *testing.T) {
	for _, values := range []map[string]any{nil, {"memory_cognify_enabled": true}, {"memory_cognify_enabled": false, "memory_cognify_command": "configured"}, {"memory_cognify_enabled": true, "memory_cognify_command": "  "}} {
		s := &postgresDataStore{settings: func() (map[string]any, error) { return values, nil }}
		if _, _, err := s.cognifySettings(); !errors.Is(err, errCognifyDisabled) {
			t.Fatal(values, err)
		}
	}
}

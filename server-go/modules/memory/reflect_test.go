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

func TestReflectionEvidence(t *testing.T) {
	records := make([]Record, 32)
	for i := range records {
		records[i] = Record{ID: 9223372036854775776 + int64(i), Key: "same", Content: fmt.Sprint(i), Tier: "L2", Kind: "fact"}
	}
	result := reflectionEvidence("query", records)
	if len(result.Results) != 32 || len(result.Contradictions) != 16 {
		t.Fatal(len(result.Results), len(result.Contradictions))
	}
	raw, _ := json.Marshal(result)
	if !strings.Contains(string(raw), "9223372036854775807") || !strings.Contains(reflectionText(result), "#9223372036854775807") {
		t.Fatal("integer precision lost")
	}
	result = reflectionEvidence("query", []Record{{ID: 1, Key: "same", Content: "same"}, {ID: 2, Key: "same", Content: "same"}, {ID: 3, Content: "other"}, {ID: 4, Content: "different"}})
	if len(result.Contradictions) != 0 {
		t.Fatal(result.Contradictions)
	}
}
func TestReflectionValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"query":" "}`, `{"query":42}`, `{"query":"x","format":"invalid"}`, `{"query":"x","scope_type":"project"}`, `{"query":"x","scope_type":"user","scope_value":"private"}`} {
		if result := runPublicCommand(t, client, "reflect", args); result["kind"] != "invalid_argument" {
			t.Fatal(args, result)
		}
	}
	if result := runPublicCommand(t, client, "reflect", `{"query":"x"}`); result["kind"] != "unavailable" {
		t.Fatal(result)
	}
}

func TestReflectionSynthesisScreening(t *testing.T) {
	backend := &postgresDataStore{settings: func() (map[string]any, error) {
		return map[string]any{"memory_cognify_enabled": true, "memory_cognify_command": "fixture"}, nil
	}}
	calls := 0
	reply := `{"narrative":"token=\u0073ecret","confidence":0.5}`
	backend.episodeCommand = func(ctx context.Context, command string, input []byte) ([]byte, error) {
		calls++
		if _, ok := ctx.Deadline(); !ok || command != "fixture" || strings.Contains(string(input), "secret") {
			t.Fatal("unscreened or unbounded model input")
		}
		return []byte(reply), nil
	}
	evidence := reflectionEvidence("token=secret", []Record{{ID: 1, Key: "token=secret", Content: "token=secret"}})
	synthesis, status := backend.synthesizeReflection(context.Background(), evidence)
	if status != "ok" || synthesis.Narrative != "[REDACTED]" || calls != 1 {
		t.Fatal(synthesis, status, calls)
	}
	evidence.Results[0].Content = "-----BEGIN PRIVATE KEY-----"
	if _, status = backend.synthesizeReflection(context.Background(), evidence); status != "screened" || calls != 1 {
		t.Fatal(status, calls)
	}
	evidence.Results[0].Content = strings.Repeat("x", episodeMaxInput)
	if _, status = backend.synthesizeReflection(context.Background(), evidence); status != "capacity" || calls != 1 {
		t.Fatal(status, calls)
	}
	evidence.Results[0].Content = "safe"
	reply = `{"narrative":"-----BEGIN PRIVATE KEY-----"}`
	if _, status = backend.synthesizeReflection(context.Background(), evidence); status != "screened" {
		t.Fatal(status)
	}
}

func exerciseReflectionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT reflection_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT reflection_replay; RELEASE SAVEPOINT reflection_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true); DELETE FROM collab_rules`)
	const largeID int64 = 9007199254741667
	longText := "reflect-fixture " + strings.Repeat("long evidence 界 ", 300)
	exec(`INSERT INTO memories(id,tier,kind,key,content,scope_type,scope_value,confidence) VALUES($1,'L2','fact','reflect-fixture',$2,'project','reflect-project',.8)`, largeID, longText)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence) SELECT 'L2','fact','reflect-fixture','reflect-fixture choice '||i,'project','reflect-project',.8 FROM generate_series(1,5)i;
 INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence) SELECT 'L2','fact','reflect-fixture','reflect-fixture private','project','reflect-private',1 FROM generate_series(1,33)i;
 SET LOCAL ROLE aimee_store_runtime`)
	bound := *backend
	enabled := true
	bound.settings = func() (map[string]any, error) {
		return map[string]any{"memory_cognify_enabled": enabled, "memory_cognify_command": "fixture-cognifier"}, nil
	}
	calls := 0
	reply := `{"narrative":"Evidence [#9007199254741667] supports this choice.","contradiction_explanation":"Different sessions.","rule_proposal":"Keep changes scoped.","confidence":0.8}`
	var runnerError error
	bound.episodeCommand = func(_ context.Context, command string, input []byte) ([]byte, error) {
		calls++
		if command != "fixture-cognifier" || strings.Contains(string(input), "reflect-fixture private") {
			t.Fatal(command, "scope escaped")
		}
		var payload struct {
			Task     string             `json:"task"`
			Memories []reflectionMemory `json:"memories"`
			N        int                `json:"nconflicts"`
		}
		if json.Unmarshal(input, &payload) != nil || payload.Task != "reflect_synthesis" || len(payload.Memories) == 0 {
			t.Fatal(string(input))
		}
		return []byte(reply), runnerError
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &bound))
	run := func(extra map[string]any) (reflectionResult, map[string]json.RawMessage) {
		t.Helper()
		args := map[string]any{"query": "reflect-fixture", "project": "reflect-project", "scope_context": true, "limit": 6}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		frame, _ := bus.EncodeCommand("reflect", raw)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var result reflectionResult
		var envelope map[string]json.RawMessage
		if json.Unmarshal(body, &result) != nil || json.Unmarshal(body, &envelope) != nil {
			t.Fatal(string(body))
		}
		return result, envelope
	}
	countRules := func() int {
		t.Helper()
		var n int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM collab_rules`).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	result, envelope := run(nil)
	if string(envelope["status"]) != `"ok"` || len(result.Results) != 6 || len(result.Contradictions) != 15 || result.Results[0].ID != largeID || result.Results[0].Content != longText || calls != 0 || countRules() != 0 {
		t.Fatal("read-only reflection", result.Results[0].ID, len(result.Results), len(result.Contradictions), envelope["status"])
	}
	_, envelope = run(map[string]any{"format": "json", "fields": "results,contradictions"})
	var rendered string
	if json.Unmarshal(envelope["output"], &rendered) != nil || !strings.Contains(rendered, "9007199254741667") || strings.Contains(rendered, `"query":`) {
		t.Fatal(rendered)
	}
	_, envelope = run(map[string]any{"format": "text"})
	if json.Unmarshal(envelope["output"], &rendered) != nil || !strings.Contains(rendered, longText) || !strings.Contains(rendered, "CONTRADICTIONS DETECTED (15)") {
		t.Fatal("text rendering lost evidence")
	}
	result, _ = run(map[string]any{"draft_rule": true})
	if result.DraftRuleID <= 0 || result.DraftStatus != "proposed" || countRules() != 1 {
		t.Fatal(result)
	}
	var state, source, text, reason string
	exec(`RESET ROLE`)
	if err := tx.QueryRow(ctx, `SELECT status,proposed_by,text,reason FROM collab_rules WHERE id=$1`, result.DraftRuleID).Scan(&state, &source, &text, &reason); err != nil || state != "proposed" || source != "reflect" || len(text) > 160 || len(reason) > 240 {
		t.Fatal(state, source, text, reason, err)
	}
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	result, _ = run(map[string]any{"limit": 2, "synthesize": true, "draft_rule": true, "command": "untrusted-caller-command"})
	if result.Synthesis == nil || result.SynthesisStatus != "ok" || result.DraftRuleID <= 0 || countRules() != 2 || calls != 1 {
		t.Fatal(result, calls)
	}
	enabled = false
	result, _ = run(map[string]any{"synthesize": true})
	if result.SynthesisStatus != "disabled" || calls != 1 || len(result.Results) != 6 {
		t.Fatal(result, calls)
	}
	enabled = true
	runnerError = errors.New("fixture unavailable")
	result, _ = run(map[string]any{"synthesize": true})
	if result.SynthesisStatus != "unavailable" || len(result.Results) != 6 {
		t.Fatal(result)
	}
	runnerError = nil
	for _, bad := range []string{`{}`, `{"narrative":"ok","confidence":2}`, `not-json`} {
		reply = bad
		result, _ = run(map[string]any{"synthesize": true})
		if result.SynthesisStatus != "invalid_response" || result.Synthesis != nil || len(result.Results) != 6 {
			t.Fatal(result)
		}
	}
	reply = `{"narrative":"ok","rule_proposal":"` + strings.Repeat("x", 161) + `","confidence":0.5}`
	result, _ = run(map[string]any{"limit": 2, "synthesize": true, "draft_rule": true})
	if result.DraftStatus != "invalid_rule" || result.DraftRuleID != 0 || countRules() != 2 {
		t.Fatal(result)
	}
	result, _ = run(map[string]any{"scope_type": "project", "scope_value": "reflect-private", "limit": 1})
	if len(result.Results) != 1 || result.Results[0].Content != "reflect-fixture private" {
		t.Fatal("exact scope", result)
	}
	result, _ = run(map[string]any{"project": "", "scope_context": false})
	if len(result.Results) != 0 {
		t.Fatal("missing identity read private memory")
	}
	exec(`INSERT INTO collab_rules(text,status) SELECT 'capacity fixture','proposed' FROM generate_series(1,48)`)
	result, _ = run(map[string]any{"draft_rule": true})
	if result.DraftStatus != "capacity" || result.DraftRuleID != 0 || countRules() != 50 {
		t.Fatal(result)
	}
	exec(`RESET ROLE; DELETE FROM collab_rules; REVOKE INSERT(text,reason,proposed_by,status) ON collab_rules FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	_, envelope = run(map[string]any{"draft_rule": true})
	if string(envelope["kind"]) != `"unavailable"` || countRules() != 0 {
		t.Fatal("draft write failure hidden", envelope)
	}
}

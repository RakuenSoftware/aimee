package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestRecallCompositionPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for composition replay")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	schema, err := os.ReadFile("../aimee/families/schema_conversation.sql")
	if err != nil {
		t.Fatal(err)
	}
	text := string(schema)
	a, b := strings.Index(text, "CREATE TABLE IF NOT EXISTS user_memories ("), strings.Index(text, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if a < 0 || b <= a {
		t.Fatal("shipping personal schema missing")
	}
	fixture := pgx.Identifier{fmt.Sprintf("private_composition_%d", time.Now().UnixNano())}.Sanitize()
	exec("CREATE SCHEMA " + fixture + "; SET LOCAL search_path=" + fixture + ",public")
	exec(text[a:b])
	full := strings.Repeat("個人設定", 600)
	exec(`INSERT INTO user_memories(id,tier,kind,key,content) VALUES
 (9007199254740993,'L3','fact','identity:name',$1),
 (42,'L2','preference','editor','private editor'),
 (43,'L2','preference','expired','expired secret'),
 (44,'L2','fact','identity:archived','archived secret')`, full)
	exec(`UPDATE user_memories SET valid_until=now() WHERE id=43;
 UPDATE user_memories SET lifecycle_state='archived' WHERE id=44`)
	for _, name := range []string{"schema_personal_memory_changes.sql", "schema_personal_memory_versions.sql", "schema_personal_memory_acl.sql", "schema_personal_memory_authority.sql", "schema_personal_memory_proposals.sql"} {
		migration, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		exec(string(migration))
	}
	shared := recallBundle{Identity: recallItems([]Record{{ID: 51, Key: "identity:name", Content: "shared name"}, {ID: 52, Key: "identity:name", Content: "duplicate shared name"}, {ID: 42, Key: "identity:team", Content: "shared team"}}), Preferences: recallItems([]Record{{ID: 61, Key: "editor", Content: "shared editor"}}), ActiveContext: []RecallRecord{}, OpenCommitments: []RecallRecord{}, AlwaysOnRules: []recallRule{{ID: 91, Title: "hard policy"}}, Reminders: []recallReminder{}, Directives: []recallDirective{}, Explain: []any{}, LimitTokens: 8192}
	for i := range shared.Identity {
		shared.Identity[i].ActivationManaged = true
	}
	sharedJSON, _ := json.Marshal(map[string]any{"status": "ok", "recall": shared, "receipt": json.RawMessage(`{"id":9007199254740995}`)})
	backend, _ := NewPostgresDataStore(evalQueryer{tx}, PlacementServer)
	handler := NewHandler(nil, WithDataStore(PlacementServer, backend))
	exercisePrivateCommandEnvelopes(t, handler, tx, full)
	call := func(input string, limit int) ([]byte, bus.ModuleStatus) {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"operation": "compose-recall", "shared_json": input, "limit_tokens": limit})
		frame, _ := bus.EncodeCommand("runtime", args)
		raw, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		body, err := bus.DecodeCommandResult(raw)
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct{ JSON string }
		if json.Unmarshal(body, &envelope) != nil || envelope.JSON == "" {
			t.Fatalf("missing composition %s", body)
		}
		return []byte(envelope.JSON), status
	}
	personal := runHostRuntime(t, handler, `{"operation":"personal-recall","limit_tokens":8192,"task_hint":"identity:name"}`)
	personalJSON, ok := personal["json"].(string)
	if !ok || !strings.Contains(personalJSON, `"id":9007199254740993`) || !strings.Contains(personalJSON, `"store":"user"`) || !strings.Contains(personalJSON, full) || strings.Contains(personalJSON, "shared name") {
		t.Fatal("personal recall envelope lost", personal)
	}
	raw, status := call(string(sharedJSON), 8192)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	var envelope struct {
		Status string
		Store  string
		Recall json.RawMessage
	}
	if json.Unmarshal(raw, &envelope) != nil || envelope.Status != "ok" || envelope.Store != "composed" {
		t.Fatal(string(raw))
	}
	var got recallBundle
	if err := json.Unmarshal(envelope.Recall, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Identity) != 2 || got.Identity[0].ID != 9007199254740993 || got.Identity[0].Content != full || got.Identity[0].Handle != "user:memory:9007199254740993" || got.Identity[0].ActivationManaged || got.Identity[1].ID != 42 || !got.Identity[1].ActivationManaged {
		t.Fatal("identity precedence/precision", got.Identity)
	}
	if len(got.Preferences) != 1 || got.Preferences[0].Content != "private editor" || got.Preferences[0].ID != 42 {
		t.Fatal(got.Preferences)
	}
	if !strings.Contains(string(raw), `"id":9007199254740995`) || strings.Contains(string(raw), "secret") || strings.Contains(string(raw), "shared name") {
		t.Fatal(string(raw))
	}
	if got.ApproxTokens != (len(envelope.Recall)+3)/4 || got.UsedTokens != got.ApproxTokens || got.BudgetExceeded {
		t.Fatal("incorrect composed budget", got.ApproxTokens)
	}
	for _, limit := range []int{128, 512} {
		raw, status = call(string(sharedJSON), limit)
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &envelope) != nil || json.Unmarshal(envelope.Recall, &got) != nil {
			t.Fatal(status, string(raw))
		}
		if got.ApproxTokens != (len(envelope.Recall)+3)/4 || got.BudgetExceeded != (got.ApproxTokens > limit) {
			t.Fatal("final composition budget", limit, string(raw))
		}
	}
	raw, status = call(string(sharedJSON), 64)
	if status != bus.ModuleStatusOK || !strings.Contains(string(raw), `"kind":"protected_context_overflow"`) || strings.Contains(string(raw), `"recall"`) {
		t.Fatal("composition dropped required rule", status, string(raw))
	}
	// A failed shared read never turns into success using personal content.
	for _, input := range []string{`{"status":"error","kind":"protected_context_overflow"}`, `{"status":"error","kind":"unavailable"}`, `{"status":"quarantined","recall":{}}`} {
		raw, status = call(input, 8192)
		if status != bus.ModuleStatusOK || string(raw) != input {
			t.Fatal("shared refusal lost", string(raw), status)
		}
	}
	for _, input := range []string{`{`, `{"status":"ok","recall":{}}`, `{"status":"unknown"}`} {
		raw, status = call(input, 8192)
		if status == bus.ModuleStatusOK && !strings.Contains(string(raw), `"status":"error"`) {
			t.Fatal("bad shared input admitted", string(raw))
		}
	}
	// Missing private storage must refuse the whole composition, never return a
	// plausible shared-only result or partial personal section.
	exec(`SAVEPOINT composition_failure; ALTER TABLE user_memories RENAME TO unavailable_personal`)
	raw, status = call(string(sharedJSON), 8192)
	if status != bus.ModuleStatusOK || !strings.Contains(string(raw), `"kind":"unavailable"`) || strings.Contains(string(raw), `"recall"`) {
		t.Fatal("private failure hidden", string(raw), status)
	}
	personal = runHostRuntime(t, handler, `{"operation":"personal-recall","limit_tokens":8192}`)
	personalJSON, _ = personal["json"].(string)
	if !strings.Contains(personalJSON, `"kind":"unavailable"`) || strings.Contains(personalJSON, `"recall"`) {
		t.Fatal("personal failure hidden", personal)
	}
	exec(`ROLLBACK TO SAVEPOINT composition_failure; RELEASE SAVEPOINT composition_failure`)
	raw, status = call(string(sharedJSON), 8192)
	if status != bus.ModuleStatusOK || !strings.Contains(string(raw), `"store":"composed"`) {
		t.Fatal("composition did not recover", string(raw), status)
	}
	{
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"personal-recall"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("plugin personal recall", status)
		}
		kb := NewHandler(nil, WithDataStore(PlacementKB, nil))
		if _, status := kb(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal("KB personal recall", status)
		}
	}
	// The host route cannot be invoked by plugins or by the KB placement.
	args, _ := json.Marshal(map[string]any{"operation": "compose-recall", "shared_json": string(sharedJSON)})
	frame, _ := bus.EncodeCommand("runtime", args)
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("plugin composition", status)
	}
	kb := NewHandler(nil, WithDataStore(PlacementKB, nil))
	if _, status := kb(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("KB composition", status)
	}
	data := dataRequest(t, DataRequest{Operation: "compose-recall", SharedRecall: sharedJSON})
	if _, status := handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: 73}, data); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("plugin data composition", status)
	}
}

// Exercise the private commands against the shipping schema and adjacent IDs
// that collapse to the same double in a native JSON decode/re-encode.
func exercisePrivateCommandEnvelopes(t *testing.T, handler bus.ModuleHandler, tx pgx.Tx, content string) {
	t.Helper()
	ctx := context.Background()
	exec := func(sql string) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql); err != nil {
			t.Fatal(err)
		}
	}
	call := func(args string) string {
		t.Helper()
		outer, status := invokeContextCommand(t, handler, 0, bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "fixture:user", TransportIdentity: "fixture:http"}, "runtime", args)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, ok := outer["json"].(string)
		if !ok || !json.Valid([]byte(body)) {
			t.Fatal("private command lost its owner envelope", outer)
		}
		return body
	}
	exec(`SAVEPOINT private_envelopes;
INSERT INTO user_memories(id,key,content) VALUES (9007199254740992,'neighbor','unchanged');
ALTER TABLE user_memories ALTER COLUMN id RESTART WITH 9007199254741001`)
	for _, args := range []string{
		`{"operation":"user-get","id":"9007199254740993","project":"forged","scope":{"type":"global"}}`,
		`{"operation":"user-list"}`,
		`{"operation":"user-search","keywords":["identity:name"]}`,
	} {
		body := call(args)
		if !strings.Contains(body, `"id":9007199254740993`) || !strings.Contains(body, content) {
			t.Fatal("private read lost integer/content precision", body)
		}
	}
	for _, id := range []string{`9007199254740992`, `9007199254740993`, `"09007199254740993"`, `"9223372036854775808"`, `"+42"`, `"42 "`, `1.5`, `null`} {
		for _, args := range []string{
			`{"operation":"user-get","id":` + id + `}`,
			`{"operation":"user-delete","id":` + id + `}`,
			`{"operation":"user-supersede","old_id":` + id + `,"new_content":"invalid change"}`,
		} {
			if body := call(args); !strings.Contains(body, `"kind":"invalid_argument"`) {
				t.Fatal("unsafe private ID admitted", args, body)
			}
		}
	}
	for _, test := range []struct{ args, want string }{
		{`{"operation":"user-store","key":"new","content":"private create"}`, `"id":9007199254741001`},
		{`{"operation":"user-supersede","old_id":"9007199254740993","new_content":"private replacement"}`, `"id":9007199254740993`},
		{`{"operation":"user-delete","id":"9007199254740993"}`, `"id":9007199254740993`},
		{`{"operation":"user-get","id":"9007199254740993"}`, `"kind":"not_found"`},
		{`{"operation":"user-get","id":"9007199254740992"}`, `"content":"unchanged"`},
		{`{"operation":"user-stats"}`, `"stats":`},
	} {
		if body := call(test.args); !strings.Contains(body, test.want) {
			t.Fatal(test.args, body)
		}
	}
	exec(`SAVEPOINT private_outage; ALTER TABLE user_memories RENAME TO unavailable_commands`)
	for _, args := range []string{
		`{"operation":"user-get","id":"9007199254740992"}`,
		`{"operation":"user-list"}`,
		`{"operation":"user-search","keywords":["neighbor"]}`,
		`{"operation":"user-store","key":"new","content":"failed create"}`,
		`{"operation":"user-supersede","old_id":"9007199254740992","new_content":"failed replacement"}`,
		`{"operation":"user-delete","id":"9007199254740992"}`,
		`{"operation":"user-stats"}`,
	} {
		if body := call(args); !strings.Contains(body, `"kind":"unavailable"`) {
			t.Fatal("private outage disguised", args, body)
		}
	}
	exec(`ROLLBACK TO SAVEPOINT private_outage; RELEASE SAVEPOINT private_outage`)
	if body := call(`{"operation":"user-get","id":"9007199254740992"}`); !strings.Contains(body, `"content":"unchanged"`) {
		t.Fatal("private owner did not recover", body)
	}
	exec(`ROLLBACK TO SAVEPOINT private_envelopes; RELEASE SAVEPOINT private_envelopes`)
}

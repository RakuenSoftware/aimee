package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestSearchViewScope(t *testing.T) {
	for _, tt := range []struct{ raw, kind, value string }{
		{`{"scope":{"workspace":"team","project":"app","session":"s","user":"u"}}`, ScopeWorkspace, "team"},
		{`{"scope":{"workspace":"current","project":"app","session":"s"}}`, ScopeProject, "app"},
		{`{"scope":{"workspace":"any","project":false,"session":"s","user":"u"}}`, "session", "s"},
		{`{"scope":{"session":"","user":"u"}}`, ScopeUser, "u"},
		{`{"scope":{"workspace":"any","project":"current"}}`, "", ""},
		{`{"scope":false}`, "", ""}, {`null`, "", ""}, {`[]`, "", ""},
	} {
		scope, ok := searchViewScope(commandArgs{"filter": json.RawMessage(tt.raw)})
		if scope.Type != tt.kind || scope.Value != tt.value || ok != (tt.kind != "") {
			t.Fatalf("%s: %+v %v", tt.raw, scope, ok)
		}
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, kind := range []string{"session", "user"} {
		r := runPublicCommand(t, client, "find_facts_visible", `{"query":"q","format":"mcp","filter":{"scope":{"`+kind+`":"foreign"}}}`)
		if r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	r := runPublicCommand(t, client, "find_facts_visible", `{"query":"q","format":"mcp"}`)
	if r["kind"] != "unavailable" || r["text"] != nil {
		t.Fatal(r)
	}
}

func TestMemorySearchText(t *testing.T) {
	if got := memorySearchText("query", nil, false); got != "No facts found for 'query'" {
		t.Fatal(got)
	}
	content := strings.Repeat("界", 5000) + " tail-marker"
	records := []publicMemoryRecord{{Key: "key", Tier: "L2", Kind: "fact", Content: content}, {Key: "second", Tier: "L1", Kind: "decision", Content: "decided"}}
	want := "Active project context is unavailable; showing shared/global memory only.\n\nFound 2 fact(s):\n\n- **key** [L2/fact]: " + content + "\n- **second** [L1/decision]: decided\n"
	if got := memorySearchText("query", records, true); got != want {
		t.Fatalf("text mismatch: got %d bytes, want %d", len(got), len(want))
	}
}

func exerciseSearchViewReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT search_view`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT search_view; RELEASE SAVEPOINT search_view`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	content := strings.Repeat("search-view-fixture 界 ", 800) + "tail-marker"
	seed := func(scope, value, key, body string) {
		t.Helper()
		exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence) VALUES('L2','fact',$1,$2,$3,$4,0.9)`, key, body, scope, value)
	}
	seed("project", "search-view-project", "local-search-view-fixture", content)
	seed("workspace", "search-view-workspace", "workspace-search-view-fixture", "workspace")
	seed("project", "search-view-private", "private-search-view-fixture", "private")
	for i := 0; i < 24; i++ {
		seed("global", "_global", "global-search-view-fixture", "global")
	}
	run := func(args map[string]any) map[string]any {
		t.Helper()
		args["query"] = "search-view-fixture"
		if _, ok := args["format"]; !ok {
			args["format"] = "mcp"
		}
		raw, err := json.Marshal(args)
		if err != nil {
			t.Fatal(err)
		}
		frame, _ := bus.EncodeCommand("find_facts_visible", raw)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var result map[string]any
		if err = json.Unmarshal(body, &result); err != nil || result["status"] != "ok" {
			t.Fatal(string(body), err)
		}
		return result
	}
	result := run(map[string]any{"project": "search-view-project", "workspace": "search-view-workspace", "limit": 2})
	want := "Found 2 fact(s):\n\n- **local-search-view-fixture** [L2/fact]: " + content + "\n- **workspace-search-view-fixture** [L2/fact]: workspace\n"
	if result["text"] != want || result["active_context_missing"] != false {
		t.Fatal("scope order/full text", result)
	}
	for _, args := range []map[string]any{{}, {"project": "__aimee_scope_missing__", "workspace": "__aimee_scope_missing__"}} {
		result = run(args)
		text := result["text"].(string)
		if result["active_context_missing"] != true || !strings.HasPrefix(text, "Active project context is unavailable;") || strings.Contains(text, "local-search") || strings.Contains(text, "workspace-search") || strings.Contains(text, "private-search") {
			t.Fatal(result)
		}
	}
	result = run(map[string]any{"include_all": true, "limit": 64})
	if result["active_context_missing"] != false || !strings.Contains(result["text"].(string), "private-search-view-fixture") {
		t.Fatal(result)
	}
	result = run(map[string]any{"project": "search-view-project", "include_all": true, "filter": map[string]any{"scope": map[string]any{"workspace": "search-view-workspace", "project": "search-view-private"}}})
	if result["text"] != "Found 1 fact(s):\n\n- **workspace-search-view-fixture** [L2/fact]: workspace\n" {
		t.Fatal(result)
	}
	result = run(map[string]any{"project": "search-view-project", "limit": 1, "filter": map[string]any{"scope": map[string]any{"workspace": "any", "project": "current"}}})
	if result["text"] != "Found 1 fact(s):\n\n- **local-search-view-fixture** [L2/fact]: "+content+"\n" {
		t.Fatal(result)
	}
	result = run(map[string]any{"project": "search-view-empty", "filter": map[string]any{"scope": map[string]any{"project": "search-view-empty"}}})
	if result["text"] != "No facts found for 'search-view-fixture'" || result["active_context_missing"] != false {
		t.Fatal(result)
	}
	// The existing structured visible API remains bounded to the visible set.
	result = run(map[string]any{"format": "", "include_all": true, "limit": 64})
	if _, ok := result["text"]; ok {
		t.Fatal(result)
	}
	for _, v := range result["facts"].([]any) {
		if v.(map[string]any)["key"] != "global-search-view-fixture" {
			t.Fatal(v)
		}
	}
}

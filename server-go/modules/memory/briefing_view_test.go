package memory

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestBriefingText(t *testing.T) {
	full := strings.Repeat("界", 4000) + " tail-marker"
	b := briefingBundle{Facts: []briefingFact{{RecallRecord: recallItems([]Record{{ID: math.MaxInt64, Tier: "L2", Kind: "fact", Content: full}})[0]}}, Activity: []briefingActivity{{SessionID: "session", Summary: "summary"}}, Entities: []briefingEntity{{Name: "Atlas", Mentions: 2}}, ApproxTokens: 100, LimitTokens: 3000}
	want := "# Session Briefing\n\n## Key Facts (1)\n  - [L2/fact #9223372036854775807] " + full + "\n\n## Recent Activity (1)\n  - session: summary\n\n## Active Entities (1)\n  - Atlas (mentions=2)\n\napprox_tokens=100 / limit_tokens=3000\n"
	if got := briefingText(b); got != want {
		t.Fatal("text mismatch")
	}
	empty := briefingBundle{LimitTokens: 1024, ApproxTokens: 42}
	if got := briefingText(empty); got != "# Session Briefing\n\n## Key Facts (0)\n\n## Recent Activity (0)\n\n## Active Entities (0)\n\napprox_tokens=42 / limit_tokens=1024\n" {
		t.Fatal(got)
	}
}

func TestMemoryJSONOutput(t *testing.T) {
	payload := json.RawMessage(`{"key_facts":[{"memory_id":9223372036854775807,"text":"日本語","created_at":"old","scope":{"description":"omit","value":"app"}}],"recent_activity":[{"created_at":"old","summary":"keep"}],"status":"ok","limit_tokens":1024}`)
	out, err := memoryJSONOutput(payload, commandArgs{"profile": json.RawMessage(`"compact"`), "fields": json.RawMessage(`"key_facts, recent_activity"`)})
	if err != nil || strings.Contains(out, "created_at") || strings.Contains(out, "description") || strings.Contains(out, "limit_tokens") || !strings.Contains(out, "9223372036854775807") || !strings.Contains(out, `"summary":"keep"`) || !strings.Contains(out, `"status":"ok"`) {
		t.Fatal(out, err)
	}
	out, err = memoryJSONOutput(payload, commandArgs{"fields": json.RawMessage(`""`)})
	if err != nil || out != `{"status":"ok"}` {
		t.Fatal(out, err)
	}
	out, err = memoryJSONOutput(payload, commandArgs{})
	if err != nil || out != string(payload) {
		t.Fatal(out, err)
	}
	out, err = memoryBundleOutput("briefing", payload, commandArgs{"format": json.RawMessage(`"mcp"`)}, true)
	if err != nil || !strings.Contains(out, `"active_context_missing":true`) || !strings.Contains(out, "9223372036854775807") || !strings.Contains(out, "created_at") {
		t.Fatal(out, err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, format := range []string{"text", "json", "mcp"} {
		r := runPublicCommand(t, client, "briefing", `{"format":"`+format+`"}`)
		if r["kind"] != "unavailable" || r["output"] != nil {
			t.Fatal(r)
		}
	}
	r := runPublicCommand(t, client, "briefing", `{"format":"invalid"}`)
	if r["kind"] != "invalid_argument" {
		t.Fatal(r)
	}
}

func exerciseBriefingViewsReplay(t *testing.T, handler bus.ModuleHandler, b briefingBundle) {
	t.Helper()
	client := clientForHandler(t, handler)
	run := func(args string) string {
		t.Helper()
		r := runPublicCommand(t, client, "briefing", args)
		if r["status"] != "ok" {
			t.Fatal(r)
		}
		output, ok := r["output"].(string)
		if !ok {
			t.Fatal(r)
		}
		return output
	}
	text := run(`{"format":"text","scope_context":true,"project":"brief-project"}`)
	if text != briefingText(b) {
		t.Fatal(text)
	}
	full := run(`{"format":"json","scope_context":true,"project":"brief-project"}`)
	var actual briefingBundle
	if json.Unmarshal([]byte(full), &actual) != nil || actual.LimitTokens != b.LimitTokens || len(actual.Facts) != len(b.Facts) || actual.Facts[0].MemoryID != b.Facts[0].MemoryID {
		t.Fatal(full)
	}
	filtered := run(`{"format":"json","profile":"compact","fields":"key_facts,recent_activity","scope_context":true,"project":"brief-project"}`)
	if strings.Contains(filtered, "created_at") || strings.Contains(filtered, "active_entities") || !strings.Contains(filtered, "local episode") || !strings.Contains(filtered, `"memory_id"`) {
		t.Fatal(filtered)
	}
	for _, args := range []string{
		`{"format":"mcp"}`,
		`{"format":"mcp","scope_context":true,"project":"__aimee_scope_missing__","workspace":"__aimee_scope_missing__"}`,
	} {
		out := run(args)
		if !strings.Contains(out, `"active_context_missing":true`) || strings.Contains(out, "brief:local") || strings.Contains(out, "brief:private") {
			t.Fatal(out)
		}
	}
	out := run(`{"format":"mcp","project":"brief-project"}`)
	if !strings.Contains(out, `"active_context_missing":false`) || !strings.Contains(out, "brief:local") || strings.Contains(out, "brief:private") {
		t.Fatal(out)
	}
	out = run(`{"format":"mcp","include_all":true}`)
	if !strings.Contains(out, `"active_context_missing":false`) || !strings.Contains(out, "brief:private") {
		t.Fatal(out)
	}
}

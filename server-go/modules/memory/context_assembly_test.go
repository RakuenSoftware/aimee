package memory

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
)

func TestContextAssemblyExplainsRenderedRows(t *testing.T) {
	rows := []Record{
		{ID: math.MaxInt64, Scope: Scope{Type: ScopeProject, Value: "app"}, Tier: "L2", Kind: "fact", Key: "cert", Content: "PostgreSQL client certificates 日本語", Confidence: .1},
		{ID: 2, Scope: Scope{Type: ScopeGlobal, Value: "_global"}, Tier: "L2", Kind: "fact", Key: "style", Content: "concise responses", Confidence: 1},
	}
	result := assembleMemoryContext(rows, "cert", "")
	if result.Context != renderMemoryContext(rows, "") || result.Budget.UsedTokens != (len(result.Context)+3)/4 || result.Budget.BudgetTokens != 0 {
		t.Fatal(result)
	}
	if len(result.Candidates) != 2 || result.Candidates[0].ID != "9223372036854775807" || result.Candidates[0].Rank != 1 || !result.Candidates[0].Selected {
		t.Fatal(result)
	}
	if result.Candidates[0].ScoreSource != "text_diagnostic" || result.Candidates[0].Score <= 0 || result.Candidates[1].Score != 0 {
		t.Fatal(result.Candidates)
	}
	rows[0].Confidence = 1
	if other := assembleMemoryContext(rows, "cert", ""); other.Candidates[0] != result.Candidates[0] {
		t.Fatal("confidence changed retrieval explanation")
	}
	raw, err := json.Marshal(result)
	if err != nil || !strings.Contains(string(raw), `"id":"9223372036854775807"`) {
		t.Fatalf("%s: %v", raw, err)
	}
	text := result.ExplainText()
	for _, needle := range []string{"9223372036854775807", "SELECTED", "no token cap", "upstream exclusions are not reported", result.Context} {
		if !strings.Contains(text, needle) {
			t.Fatalf("missing %q in %s", needle, text)
		}
	}
	empty := assembleMemoryContext(nil, "", "")
	raw, err = json.Marshal(empty)
	if err != nil || !strings.Contains(string(raw), `"candidates":[]`) || empty.Context != "# Memory Context\n" {
		t.Fatalf("%s: %v", raw, err)
	}
}

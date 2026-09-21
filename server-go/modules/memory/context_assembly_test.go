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

func TestContextAssemblyWholeRowByteAllocation(t *testing.T) {
	rows := []Record{{ID: math.MaxInt64, Key: "first", Content: "complete 界🦊 content"}, {ID: 2, Key: "second", Content: "second whole record"}}
	full := renderMemoryContext(rows, "界")
	for _, limit := range []int{0, 1, len(full) - 1, len(full), len(full) + 1} {
		got := assembleMemoryContextWithBudget(rows, "", "界", &limit)
		if len(got.Context) > limit || got.Budget.UsedBytes != len(got.Context) || *got.Budget.BudgetBytes != limit {
			t.Fatal(got)
		}
		selected := 0
		for _, c := range got.Candidates {
			if c.Selected {
				selected++
			}
		}
		if selected+got.Budget.RejectedForBudget != len(rows) {
			t.Fatal(got)
		}
		if limit >= len(full) && (selected != 2 || got.Context != full) {
			t.Fatal("exact fit changed", got)
		}
		if limit == len(full)-1 && (selected != 1 || got.Context != renderMemoryContext(rows[:1], "界") || !strings.Contains(got.ExplainText(), "BUDGET")) {
			t.Fatal("partial row retained", got)
		}
		if limit < 2 && (selected != 0 || got.Context != "") {
			t.Fatal("zero allocation bypassed", got)
		}
	}
}

func TestDataAssemblyBudgetValidation(t *testing.T) {
	for _, raw := range []string{"null", "-1", "1.5", `"12"`, "1048577"} {
		if _, err := decodeDataRequest([]byte(`{"operation":"assemble-context","assembly_budget_bytes":` + raw + `}`)); err == nil {
			t.Fatal(raw)
		}
	}
	if _, err := decodeDataRequest([]byte(`{"operation":"get","assembly_budget_bytes":0}`)); err == nil {
		t.Fatal("unsupported budget ignored")
	}
	for _, raw := range []string{"0", "1024"} {
		got, err := decodeDataRequest([]byte(`{"operation":"assemble-context","assembly_budget_bytes":` + raw + `}`))
		if err != nil || got.assemblyBytes == nil {
			t.Fatal(got, err)
		}
	}
}

func BenchmarkContextAssemblyProjection(b *testing.B) {
	rows := make([]Record, 12)
	for i := range rows {
		rows[i] = Record{ID: int64(i + 1), Key: "release", Content: strings.Repeat("release deployment with complete Unicode 界🦊 context. ", 80)}
	}
	limit := 8192
	b.Run("with_diagnostics", func(b *testing.B) {
		b.ReportAllocs()
		for i := 0; i < b.N; i++ {
			r := assembleMemoryContextWithBudget(rows, "release deployment", "", &limit)
			if r.Budget.UsedBytes == 0 {
				b.Fatal("empty")
			}
		}
	})
	b.Run("projection_only", func(b *testing.B) {
		b.ReportAllocs()
		for i := 0; i < b.N; i++ {
			text, n := renderMemoryContextBounded(rows, "", &limit)
			if len(text) == 0 || n == 0 {
				b.Fatal("empty")
			}
		}
	})
}

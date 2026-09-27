package memory

import (
	"fmt"
	"strconv"
	"strings"
)

// ContextAssembly describes the exact rows rendered by the shared owner. The
// assembly command limits rows and optionally exact bytes. A zero byte budget
// retains no content; the legacy zero token budget means no token cap. Scores are text diagnostics, not a replacement for the store's ordering
// (which also accounts for scope, graph, and negation).
type ContextAssembly struct {
	Context    string                     `json:"context"`
	Budget     ContextAssemblyBudget      `json:"budget"`
	Candidates []ContextAssemblyCandidate `json:"candidates"`
}

type ContextAssemblyBudget struct {
	BudgetBytes            *int `json:"budget_bytes,omitempty"`
	UsedBytes              int  `json:"used_bytes"`
	BudgetTokens           int  `json:"budget_tokens"`
	UsedTokens             int  `json:"used_tokens"`
	RejectedForBudget      int  `json:"rejected_for_budget"`
	DeferredForOriginQuota int  `json:"deferred_for_origin_quota"`
	HeldForActivation      int  `json:"held_for_activation"`
}

type ContextAssemblyCandidate struct {
	// Decimal strings survive the native CLI's JSON transport without rounding.
	ID            string  `json:"id"`
	Tier          string  `json:"tier"`
	Kind          string  `json:"kind"`
	Key           string  `json:"key"`
	Scope         string  `json:"scope"`
	Rank          int     `json:"rank"`
	Score         float64 `json:"score"`
	ScoreSource   string  `json:"score_source"`
	Tokens        int     `json:"tokens"`
	ScorePerToken float64 `json:"score_per_token"`
	Selected      bool    `json:"selected"`
}

func assembleMemoryContext(records []Record, query, blockType string) ContextAssembly {
	return assembleMemoryContextWithBudget(records, query, blockType, nil)
}

func assembleMemoryContextWithBudget(records []Record, query, blockType string, limit *int) ContextAssembly {
	text, retained := renderMemoryContextBounded(records, blockType, limit)
	result := ContextAssembly{Context: text, Candidates: make([]ContextAssemblyCandidate, 0, len(records))}
	result.Budget.BudgetBytes = limit
	result.Budget.UsedBytes = len(text)
	result.Budget.RejectedForBudget = len(records) - retained
	result.Budget.UsedTokens = (len(result.Context) + 3) / 4
	for i, record := range records {
		tokens := (len(fmt.Sprintf("\n- [#%d] %s: %s", record.ID, record.Key, record.Content)) + 3) / 4
		score := rankText(rankingInput{record.Key, record.Content}, query).Total
		result.Candidates = append(result.Candidates, ContextAssemblyCandidate{
			ID: strconv.FormatInt(record.ID, 10), Tier: record.Tier, Kind: record.Kind, Key: record.Key, Scope: record.Scope.Type,
			Rank: i + 1, Score: score, ScoreSource: "text_diagnostic", Tokens: tokens, ScorePerToken: score / float64(tokens), Selected: i < retained,
		})
	}
	return result
}

func (a ContextAssembly) ExplainText() string {
	var out strings.Builder
	fmt.Fprintf(&out, "## Context Assembly Explain\nused: %d estimated tokens | no token cap\nScores: text diagnostics; rank is the final retrieval order.\nCandidates: returned rows only; upstream exclusions are not reported.\n\n", a.Budget.UsedTokens)
	if a.Budget.BudgetBytes != nil {
		fmt.Fprintf(&out, "Byte allocation: %d | used: %d | omitted rows: %d\n\n", *a.Budget.BudgetBytes, a.Budget.UsedBytes, a.Budget.RejectedForBudget)
	}
	fmt.Fprintf(&out, "%-8s %-4s %-12s %-6s %-10s %-8s %-8s %-6s\n", "ID", "Tier", "Kind", "Tok", "Score/Tok", "Score", "Scope", "Status")
	out.WriteString("------------------------------------------------------------------------\n")
	for _, c := range a.Candidates {
		status := "SELECTED"
		if !c.Selected {
			status = "BUDGET"
		}
		fmt.Fprintf(&out, "%-8s %-4s %-12s %-6d %-10.4f %-8.4f %-8s %s\n", c.ID, c.Tier, c.Kind, c.Tokens, c.ScorePerToken, c.Score, c.Scope, status)
	}
	fmt.Fprintf(&out, "\n--- Assembled Context ---\n%s\n", a.Context)
	return out.String()
}

package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strings"
	"time"
)

type reflectionOptions struct {
	DraftRule  bool `json:"draft_rule"`
	Synthesize bool `json:"synthesize"`
}
type reflectionMemory struct {
	ID         int64   `json:"id"`
	Tier       string  `json:"tier"`
	Kind       string  `json:"kind"`
	Key        string  `json:"key"`
	Content    string  `json:"content"`
	Confidence float64 `json:"confidence"`
}
type reflectionConflict struct {
	A   int64  `json:"id_a"`
	B   int64  `json:"id_b"`
	Key string `json:"key"`
}
type reflectionSynthesis struct {
	Narrative   string  `json:"narrative"`
	Explanation string  `json:"contradiction_explanation"`
	Rule        string  `json:"rule_proposal"`
	Confidence  float64 `json:"confidence"`
}
type reflectionResult struct {
	Query           string               `json:"query"`
	Results         []reflectionMemory   `json:"results"`
	Contradictions  []reflectionConflict `json:"contradictions"`
	DraftRuleID     int64                `json:"draft_rule_id,omitempty"`
	DraftStatus     string               `json:"draft_rule_status,omitempty"`
	Synthesis       *reflectionSynthesis `json:"synthesis,omitempty"`
	SynthesisStatus string               `json:"synthesis_status,omitempty"`
}

func reflectionEvidence(query string, records []Record) reflectionResult {
	out := reflectionResult{Query: query, Results: []reflectionMemory{}, Contradictions: []reflectionConflict{}}
	for _, r := range records {
		out.Results = append(out.Results, reflectionMemory{r.ID, r.Tier, r.Kind, r.Key, r.Content, r.Confidence})
	}
	// Bound the entire nested scan. The native loop could overflow its 16-pair
	// array when many records shared a key in the same outer iteration.
	for i, a := range out.Results {
		for _, b := range out.Results[i+1:] {
			if len(out.Contradictions) == 16 {
				return out
			}
			if a.Key != "" && a.Key == b.Key && a.Content != b.Content {
				out.Contradictions = append(out.Contradictions, reflectionConflict{a.ID, b.ID, a.Key})
			}
		}
	}
	return out
}

func (s *postgresDataStore) reflectMemories(ctx context.Context, request DataRequest, exact bool) (reflectionResult, error) {
	var records []Record
	var err error
	if exact {
		records, err = s.Search(ctx, request.Scope, request.Query, "", "", request.Limit)
	} else {
		records, err = s.SearchVisible(ctx, request)
	}
	if err != nil {
		return reflectionResult{}, err
	}
	result := reflectionEvidence(request.Query, records)
	options := request.Reflection
	if options.DraftRule && len(records) >= 5 {
		result.DraftRuleID, result.DraftStatus, err = s.proposeReflectionRule(ctx, "Recurring pattern detected for: "+textBound(request.Query, 120), fmt.Sprintf("reflect found %d memories matching %q — pattern may warrant a standing rule", len(records), textBound(request.Query, 180)))
		if err != nil {
			return result, err
		}
	}
	if options.Synthesize {
		result.Synthesis, result.SynthesisStatus = s.synthesizeReflection(ctx, result)
		if result.Synthesis != nil && options.DraftRule && result.DraftRuleID == 0 && result.Synthesis.Rule != "" {
			result.DraftRuleID, result.DraftStatus, err = s.proposeReflectionRule(ctx, result.Synthesis.Rule, "reflect --synthesize proposed rule for: "+textBound(request.Query, 180))
			if err != nil {
				return result, err
			}
		}
	}
	if options.DraftRule && result.DraftStatus == "" {
		result.DraftStatus = "insufficient_evidence"
	}
	return result, nil
}

// Proposals use the existing collaborative-rule table and never activate a rule.
// Other collaborative-rule producers retain their own owner/API.
func (s *postgresDataStore) proposeReflectionRule(ctx context.Context, text, reason string) (int64, string, error) {
	if len(text) > 160 {
		return 0, "invalid_rule", nil
	}
	text, err := screenMemoryText(text)
	if err != nil {
		return 0, "", err
	}
	reason, err = screenMemoryText(reason)
	if err != nil {
		return 0, "", err
	}
	reason = textBound(reason, 240)
	if len(text) > 160 {
		return 0, "invalid_rule", nil
	}
	if text == "" {
		return 0, "", errors.New("memory: empty reflection rule")
	}
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('memory:reflection-rules',0))`); err != nil {
		return 0, "", err
	}
	var total int
	if err = s.db.QueryRow(ctx, `SELECT count(*) FROM collab_rules`).Scan(&total); err != nil {
		return 0, "", err
	}
	if total >= 50 {
		return 0, "capacity", nil
	}
	var id int64
	err = s.db.QueryRow(ctx, `INSERT INTO collab_rules(text,reason,proposed_by,status) VALUES($1,$2,'reflect','proposed') RETURNING id`, text, reason).Scan(&id)
	return id, "proposed", err
}

func (s *postgresDataStore) synthesizeReflection(ctx context.Context, result reflectionResult) (*reflectionSynthesis, string) {
	command, _, err := s.cognifySettings()
	if errors.Is(err, errCognifyDisabled) {
		return nil, "disabled"
	}
	if err != nil {
		return nil, "unavailable"
	}
	memories := append([]reflectionMemory{}, result.Results...)
	for i := range memories {
		if memories[i].Key, err = screenMemoryText(memories[i].Key); err != nil {
			return nil, "screened"
		}
		if memories[i].Content, err = screenMemoryText(memories[i].Content); err != nil {
			return nil, "screened"
		}
	}
	query, err := screenMemoryText(result.Query)
	if err != nil {
		return nil, "screened"
	}
	input, _ := json.Marshal(map[string]any{"task": "reflect_synthesis", "query": query, "memories": memories, "nconflicts": len(result.Contradictions), "instruction": "Produce a cited narrative answer synthesizing the retrieved memories. For any contradictions, explain why they might exist (different sessions, projects, or outdated data). Return JSON with fields: narrative (string, cite memory IDs as [#N]), contradiction_explanation (string, empty if none), rule_proposal (string, proposed standing rule if pattern is clear, else empty), confidence (number 0-1)."})
	if len(input) > episodeMaxInput {
		return nil, "capacity"
	}
	runner := s.episodeCommand
	if runner == nil {
		runner = runEpisodeCommand
	}
	attempt, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	raw, err := runner(attempt, command, input)
	if err != nil {
		return nil, "unavailable"
	}
	var synthesis reflectionSynthesis
	if len(raw) > episodeMaxOutput || json.Unmarshal(raw, &synthesis) != nil || strings.TrimSpace(synthesis.Narrative) == "" || math.IsNaN(synthesis.Confidence) || synthesis.Confidence < 0 || synthesis.Confidence > 1 {
		return nil, "invalid_response"
	}
	for _, field := range []*string{&synthesis.Narrative, &synthesis.Explanation, &synthesis.Rule} {
		if *field, err = screenMemoryText(*field); err != nil {
			return nil, "screened"
		}
	}
	return &synthesis, "ok"
}

func reflectionText(result reflectionResult) string {
	var out strings.Builder
	noun := "memories"
	if len(result.Results) == 1 {
		noun = "memory"
	}
	fmt.Fprintf(&out, "Reflect: %s\nFound %d relevant %s\n\n", result.Query, len(result.Results), noun)
	byID := map[int64]reflectionMemory{}
	for i, r := range result.Results {
		byID[r.ID] = r
		key := r.Key
		if key == "" {
			key = "(none)"
		}
		fmt.Fprintf(&out, "[%d] #%d  tier=%s  kind=%s\n    Key: %s\n    %s\n\n", i+1, r.ID, r.Tier, r.Kind, key, r.Content)
	}
	if len(result.Contradictions) > 0 {
		fmt.Fprintf(&out, "CONTRADICTIONS DETECTED (%d)\n", len(result.Contradictions))
		for _, c := range result.Contradictions {
			fmt.Fprintf(&out, "  Key %q has conflicting entries:\n    [#%d] %s\n    [#%d] %s\n\n", c.Key, c.A, byID[c.A].Content, c.B, byID[c.B].Content)
		}
	}
	if s := result.Synthesis; s != nil {
		fmt.Fprintf(&out, "\nSYNTHESIS (confidence=%.2f)\n%s\n", s.Confidence, s.Narrative)
		if s.Explanation != "" {
			fmt.Fprintf(&out, "\nContradiction explanation: %s\n", s.Explanation)
		}
	} else if result.SynthesisStatus != "" {
		fmt.Fprintf(&out, "Synthesis: %s\n", result.SynthesisStatus)
	}
	if result.DraftRuleID > 0 {
		fmt.Fprintf(&out, "Draft rule proposed (id=%d) — use `aimee rules approve %d` to activate.\n", result.DraftRuleID, result.DraftRuleID)
	} else if result.DraftStatus == "insufficient_evidence" {
		out.WriteString("(--draft-rule: fewer than 5 results, no rule proposed)\n")
	} else if result.DraftStatus != "" {
		fmt.Fprintf(&out, "Draft rule: %s\n", result.DraftStatus)
	}
	return out.String()
}

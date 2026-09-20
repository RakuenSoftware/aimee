package memory

import (
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

type ingressEntry struct {
	kind, header, preview string
}

// Keep the deployed grouping and footer contract while the remaining retrieval
// transports migrate. Limits count UTF-8 bytes, not characters or token guesses.
func ingressRenderBlock(entries []ingressEntry, budget, missing int) (string, int) {
	block, omitted, _, _ := ingressRenderBlockSelected(entries, budget, missing, nil)
	return block, omitted
}

func ingressRenderBlockSelected(entries []ingressEntry, budget, missing int, projections map[int]*typedContextResult) (string, int, []int, error) {
	available := max(0, budget-384)
	var block strings.Builder
	selected := []int{}
	omitted, previous, first := 0, "", true
	for i, entry := range entries {
		if i == 0 || entry.kind != previous {
			if block.Len() > 0 {
				block.WriteByte('\n')
			}
			first, previous = true, entry.kind
		}
		candidate := entry.preview
		if projection := projections[i]; projection != nil {
			remaining := available - block.Len()
			if first {
				remaining -= len(entry.header)
			}
			remaining = max(0, remaining)
			if err := projection.fitProjectionBytes(remaining); err != nil {
				return "", 0, nil, err
			}
			if len(projection.Retained) == 0 {
				omitted++
				continue
			}
			candidate = projection.Rendered
		}
		if first {
			candidate = entry.header + candidate
		}
		if candidate == "" {
			continue
		}
		if block.Len()+len(candidate) <= available {
			block.WriteString(candidate)
			selected = append(selected, i)
			first = false
		} else {
			omitted++
		}
	}
	if block.Len() > 0 {
		footer := fmt.Sprintf("context-budget: used_bytes=%d budget_bytes=%d omitted_count=%d headline_missing_count=%d\n", block.Len(), budget, omitted, missing)
		if block.Len()+len(footer) <= available {
			block.WriteString(footer)
		}
		if omitted > 0 {
			note := fmt.Sprintf("... (%d more available via get_context_block or memory_get)\n", omitted)
			if block.Len()+len(note) <= available {
				block.WriteString(note)
			}
		}
	}
	return block.String(), omitted, selected, nil
}

func ingressEnvelope(block string, score float64) string {
	if strings.Trim(block, " \t\n\r") == "" {
		return ""
	}
	band := confidenceForMicros(score * 1000000)
	confidence := map[uint32]string{ConfidenceLow: "low", ConfidenceMedium: "medium", ConfidenceHigh: "high"}[band]
	return "<aimee-context confidence=\"" + confidence + "\">\n" + ingressTerminated(block) + "</aimee-context>"
}

func ingressTerminated(text string) string {
	if !strings.HasSuffix(text, "\n") {
		return text + "\n"
	}
	return text
}

type ingressCodeHit struct {
	FilePath string `json:"file_path"`
	Snippet  string `json:"snippet"`
	Line     int    `json:"line"`
}

type ingressMemoryPreview struct {
	// Decimal text avoids losing native int64 IDs through cJSON's double.
	ID       string  `json:"id"`
	Key      string  `json:"key"`
	Tier     string  `json:"tier"`
	Kind     string  `json:"kind"`
	Headline string  `json:"headline"`
	Content  string  `json:"content"`
	Score    float64 `json:"score"`
}

// Evidence at the assembled-envelope boundary, not a provider dispatch receipt.
type ingressRetainedMemory struct {
	ID      string `json:"id"`
	Preview string `json:"preview"`
}

// These references identify occurrences in an assembled projection. They do not
// assert a canonical source revision, delivery to a provider, or task success.
type ingressProjectionEvidenceRef struct {
	Type string `json:"type"`
	Ref  string `json:"ref"`
}

type ingressAssemblyRequest struct {
	TypedRequested   bool                   `json:"typed_requested"`
	TypedContextJSON string                 `json:"typed_context_json"`
	ContextLimits    *ContextLimits         `json:"context_limits,omitempty"`
	Budget           int                    `json:"budget"`
	Compress         bool                   `json:"compress"`
	CompressMin      int                    `json:"compress_min"`
	TaskBlock        string                 `json:"task_block"`
	TaskConfidence   float64                `json:"task_confidence"`
	Code             []ingressCodeHit       `json:"code"`
	Memories         []ingressMemoryPreview `json:"memories"`
	FactsRequested   bool                   `json:"facts_requested"`
	FactsResponse    json.RawMessage        `json:"facts_response"`
	Temporal         string                 `json:"temporal"`
	Audit            string                 `json:"audit"`
}

func ingressAssemble(request ingressAssemblyRequest) (map[string]any, error) {
	if len(request.Code) > 6 || len(request.Memories) > 5 {
		return nil, fmt.Errorf("ingress retrieval limit exceeded")
	}
	if request.Budget <= 0 {
		request.Budget = 6144
	}
	var err error
	request.Budget, err = request.ContextLimits.byteLimit(request.Budget)
	if err != nil {
		return nil, err
	}
	if request.CompressMin <= 0 {
		request.CompressMin = 80
	}
	entries := make([]ingressEntry, 0, 15)
	memoryEntries := make(map[int]ingressRetainedMemory, len(request.Memories))
	codeEntries := make(map[int]int, len(request.Code))
	score, missing, folded, saved := 0.0, 0, 0, 0
	if request.TaskBlock != "" {
		entries = append(entries, ingressEntry{"code", "", request.TaskBlock})
		score = request.TaskConfidence
	}
	header := "recommended (code):\n"
	if request.Compress {
		header = "recommended (code — expand via code_span_get):\n"
	}
	for index, hit := range request.Code {
		body := "  - " + hit.FilePath + "\n"
		if request.Compress && hit.Line > 0 && len(hit.Snippet) > request.CompressMin {
			body = fmt.Sprintf("  - %s:%d\n", hit.FilePath, hit.Line)
			folded++
			saved += len(hit.Snippet)
		} else if hit.Snippet != "" {
			body += "    > " + ingressSingleLine(hit.Snippet, 150) + "\n"
		}
		codeEntries[len(entries)] = index
		entries = append(entries, ingressEntry{"code", header, body})
	}
	score = max(score, float64(len(request.Code))/6)
	for _, row := range request.Memories {
		id, err := strconv.ParseInt(row.ID, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("invalid memory preview identity")
		}
		if id <= 0 {
			continue
		}
		preview, headlineMissing := row.Headline, row.Headline == ""
		if headlineMissing {
			preview = row.Content
			missing++
		}
		body := fmt.Sprintf("  - memory:%d", id)
		if row.Key != "" {
			body += " " + ingressSingleLine(row.Key, 80)
		}
		tier, kind := row.Tier, row.Kind
		if tier == "" {
			tier = "?"
		}
		if kind == "" {
			kind = "memory"
		}
		body += fmt.Sprintf(" [%s/%s score=%.3f headline_missing=%t]\n", tier, kind, row.Score, headlineMissing)
		preview = ingressSingleLine(preview, 220)
		if preview != "" {
			body += "    > " + preview + "\n"
		}
		memoryEntries[len(entries)] = ingressRetainedMemory{ID: row.ID, Preview: preview}
		entries = append(entries, ingressEntry{"memory", "recommended (memory previews):\n", body})
	}
	if n := len(request.Memories); n > 0 {
		memoryScore := .1
		if n >= 4 {
			memoryScore = .7
		} else if n >= 2 {
			memoryScore = .4
		}
		score = max(score, memoryScore)
	}
	var facts struct {
		Status string  `json:"status"`
		Facts  *string `json:"facts"`
	}
	factsUnavailable := request.FactsRequested && (json.Unmarshal(request.FactsResponse, &facts) != nil || facts.Status != "ok" || facts.Facts == nil)
	if request.FactsRequested && !factsUnavailable && *facts.Facts != "" {
		entries = append(entries, ingressEntry{"facts", "", "## Known facts\n" + ingressTerminated(*facts.Facts)})
		score = max(score, .5)
	}
	projections := map[int]*typedContextResult{}
	var typed *typedContextResult
	sourceDigest, sourceSelection, sourceCount := "", "", 0
	typedUnavailable := request.TypedRequested && request.TypedContextJSON == ""
	if request.TypedContextJSON != "" {
		if request.Temporal != "" {
			return nil, fmt.Errorf("ambiguous typed projection inputs")
		}
		var outcome struct {
			Status string `json:"status"`
		}
		if json.Unmarshal([]byte(request.TypedContextJSON), &outcome) == nil && outcome.Status == "error" {
			typedUnavailable = true
			request.TypedContextJSON = ""
		}
	}
	if request.TypedContextJSON != "" {
		typed, err = decodeTypedProjection(request.TypedContextJSON)
		if err != nil {
			return nil, err
		}
		sourceDigest, sourceSelection, sourceCount = typed.ProjectionDigest, typed.SelectionDigest, len(typed.Retained)
		if sourceCount > 0 {
			projections[len(entries)] = typed
			entries = append(entries, ingressEntry{"temporal", "recommended (temporal learning):\n", typed.Rendered})
			score = max(score, .6)
		} else if err = typed.fitProjectionBytes(0); err != nil {
			return nil, err
		}
	} else if request.Temporal != "" {
		entries = append(entries, ingressEntry{"temporal", "recommended (temporal learning):\n", request.Temporal})
		score = max(score, .6)
	}
	if request.Audit != "" {
		entries = append(entries, ingressEntry{"audit", "", "recommended (audit context):\n" + ingressTerminated(request.Audit)})
		score = max(score, .4)
	}
	block, omitted, selected, err := ingressRenderBlockSelected(entries, request.Budget, missing, projections)
	if err != nil {
		return nil, err
	}
	envelope := ingressEnvelope(block, score)
	accounting, err := accountMemoryEnvelope(envelope, request.Budget)
	if err != nil {
		return nil, err
	}
	retained := []string{}
	retainedMemories := []ingressRetainedMemory{}
	retainedCode := []int{}
	for _, index := range selected {
		if memory, ok := memoryEntries[index]; ok {
			retained = append(retained, memory.ID)
			retainedMemories = append(retainedMemories, memory)
		}
		if codeIndex, ok := codeEntries[index]; ok {
			retainedCode = append(retainedCode, codeIndex)
		}
	}
	result := map[string]any{"status": "ok", "block": block, "envelope": envelope,
		"context_accounting": accounting, "retained_memory_ids": retained,
		"retained_memories": retainedMemories, "retained_code_indices": retainedCode,
		"omitted_count": omitted, "headline_missing_count": missing, "folded_count": folded,
		"folded_saved": saved, "facts_unavailable": factsUnavailable, "typed_unavailable": typedUnavailable}
	if typed != nil {
		refs := make([]ingressProjectionEvidenceRef, 0, len(typed.Retained))
		for _, item := range typed.Retained {
			refs = append(refs, ingressProjectionEvidenceRef{
				Type: "memory_projection_item",
				Ref:  "typed:v1:" + typed.SelectionDigest + ":" + item.Channel + ":" + item.ID,
			})
		}
		result["retained_typed_refs"] = refs
		result["typed_projection"] = map[string]any{
			"schema_version": 1, "boundary": "ingress_envelope", "source_projection_digest": sourceDigest,
			"projection_digest": typed.ProjectionDigest, "rendered_bytes": typed.RenderedBytes,
			"source_selection_digest": sourceSelection, "selection_digest": typed.SelectionDigest,
			"retained_items": typed.Retained, "omitted_count": sourceCount - len(typed.Retained),
			"context_accounting":  typed.Accounting,
			"context_sufficiency": typed.Sufficiency,
		}
	}
	return result, nil
}

func handleIngressAssembly(args commandArgs) ([]byte, bus.ModuleStatus) {
	raw, err := json.Marshal(args)
	var request ingressAssemblyRequest
	_, limitsPresent := args["context_limits"]
	if err != nil || json.Unmarshal(raw, &request) != nil || (limitsPresent && request.ContextLimits == nil) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	result, err := ingressAssemble(request)
	if err != nil {
		var refusal *contextBudgetError
		if errors.As(err, &refusal) {
			return commandResult(commandError(refusal.kind, refusal.message))
		}
		return nil, bus.ModuleStatusInvalidRequest
	}
	return commandResult(result)
}

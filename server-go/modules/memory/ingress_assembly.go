package memory

import (
	"encoding/json"
	"errors"
	"fmt"
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
	Source *typedSourceVersion `json:"source_version,omitempty"`
	// Decimal text avoids losing native int64 IDs through cJSON's double.
	ID        string  `json:"id"`
	Key       string  `json:"key"`
	Tier      string  `json:"tier"`
	Kind      string  `json:"kind"`
	Headline  string  `json:"headline"`
	Content   string  `json:"content"`
	Score     float64 `json:"score"`
	ScoreText string  `json:"score_text,omitempty"`
	Preview   string  `json:"preview,omitempty"`
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
	MemoryProjection json.RawMessage        `json:"memory_projection,omitempty"`
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
	var memoryProjection *previewProjection
	if len(request.MemoryProjection) != 0 {
		memoryProjection, err = decodePreviewProjection(request.MemoryProjection, request.Memories)
		if err != nil {
			return nil, err
		}
	} else {
		for _, row := range request.Memories {
			if row.Source != nil {
				return nil, &contextBudgetError{"invalid_projection", "memory preview commitment missing"}
			}
		}
	}
	request.Budget, err = request.ContextLimits.byteLimit(request.Budget)
	if err != nil {
		return nil, err
	}
	if request.CompressMin <= 0 {
		request.CompressMin = 80
	}
	entries := make([]ingressEntry, 0, 15)
	memoryEntries := make(map[int]ingressRetainedMemory, len(request.Memories))
	memorySourceEntries := make(map[int]ingressMemoryPreview, len(request.Memories))
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
		body, preview, headlineMissing, err := renderMemoryPreview(row)
		if err != nil {
			return nil, err
		}
		if body == "" {
			continue
		}
		if headlineMissing {
			missing++
		}
		memoryEntries[len(entries)] = ingressRetainedMemory{ID: row.ID, Preview: preview}
		memorySourceEntries[len(entries)] = row
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
		Status     string          `json:"status"`
		Facts      *string         `json:"facts"`
		Projection json.RawMessage `json:"fact_projection"`
	}
	var factSources *factProjection
	factEntry := -1
	factsUnavailable := request.FactsRequested && (json.Unmarshal(request.FactsResponse, &facts) != nil || facts.Status != "ok" || facts.Facts == nil)
	if request.FactsRequested && !factsUnavailable && len(facts.Projection) != 0 {
		if json.Unmarshal(facts.Projection, &factSources) != nil || !factSources.valid(*facts.Facts) {
			return nil, &contextBudgetError{"invalid_projection", "fact projection identity or serialized evidence mismatch"}
		}
	}
	if request.FactsRequested && !factsUnavailable && *facts.Facts != "" {
		factEntry = len(entries)
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
			Status    string `json:"status"`
			Kind      string `json:"kind"`
			ErrorType string `json:"error_type"`
		}
		if json.Unmarshal([]byte(request.TypedContextJSON), &outcome) == nil {
			// An owner refusal is authoritative. Only dependency unavailability
			// may omit this optional lane; a rejected projection must never
			// become permission to dispatch without its required context.
			if outcome.Status == "error" && outcome.Kind != "unavailable" {
				kind := outcome.Kind
				if kind == "" {
					kind = outcome.ErrorType
				}
				if kind == "" {
					kind = "invalid_projection"
				}
				return nil, &contextBudgetError{kind, "typed memory owner refused context assembly"}
			}
			// The optional KB transport uses typed non-success statuses as well
			// as the owner's error envelope. They carry no usable projection.
			// In particular a standalone Server has no shared KB to query.
			switch outcome.Status {
			case "error", "unavailable", "stale", "unauthorized", "empty", "abstained":
				typedUnavailable = true
				request.TypedContextJSON = ""
			}
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
	// Capture the candidate references before the packer mutates typed projections.
	originalTyped := []typedProjectionRef{}
	if typed != nil {
		originalTyped = append(originalTyped, typed.Retained...)
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
	retainedMemorySources := []ingressMemoryPreview{}
	retainedCode := []int{}
	factsRetained := false
	for _, index := range selected {
		if index == factEntry {
			factsRetained = true
		}
		if memory, ok := memoryEntries[index]; ok {
			retained = append(retained, memory.ID)
			retainedMemories = append(retainedMemories, memory)
			retainedMemorySources = append(retainedMemorySources, memorySourceEntries[index])
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
	selectedEntry := map[int]bool{}
	for _, i := range selected {
		selectedEntry[i] = true
	}
	dispositions := []map[string]any{}
	addDisposition := func(channel, id string, kept bool) {
		state := "budget_dropped"
		if kept {
			state = "assembled"
		}
		dispositions = append(dispositions, map[string]any{"channel": channel, "stable_id": id, "disposition": state})
	}
	for i, entry := range entries {
		if memory, ok := memoryEntries[i]; ok {
			addDisposition("memory", memory.ID, selectedEntry[i])
			continue
		}
		if code, ok := codeEntries[i]; ok {
			addDisposition("code", fmt.Sprint(code), selectedEntry[i])
			continue
		}
		if i == factEntry && factSources != nil {
			for _, ref := range factSources.Retained {
				addDisposition(ref.Channel, ref.ID, selectedEntry[i])
			}
			continue
		}
		if entry.kind != "typed" {
			addDisposition(entry.kind, fmt.Sprint(i), selectedEntry[i])
		}
	}
	if typed != nil {
		kept := map[string]bool{}
		for _, ref := range typed.Retained {
			kept[ref.Channel+":"+ref.ID] = true
		}
		for _, ref := range originalTyped {
			addDisposition(ref.Channel, ref.ID, kept[ref.Channel+":"+ref.ID])
		}
	}
	result["packing_dispositions"] = dispositions
	if memoryProjection != nil {
		final, err := newPreviewProjection(retainedMemorySources)
		if err != nil {
			return nil, err
		}
		refs := make([]ingressProjectionEvidenceRef, 0, len(final.Retained))
		for _, ref := range final.Retained {
			refs = append(refs, ingressProjectionEvidenceRef{Type: "memory_projection_item", Ref: "previews:v1:" + final.SelectionDigest + ":" + ref.Source.Kind + ":" + ref.ID})
		}
		result["retained_memory_source_refs"] = refs
		result["memory_projection"] = map[string]any{"schema_version": 1, "boundary": "ingress_envelope",
			"source_projection_digest": memoryProjection.ProjectionDigest, "source_selection_digest": memoryProjection.SelectionDigest,
			"projection_digest": final.ProjectionDigest, "selection_digest": final.SelectionDigest, "retained_items": final.Retained,
			"rendered_bytes": final.RenderedBytes, "source_version_state": final.SourceVersionState, "omitted_count": len(memoryProjection.Retained) - len(final.Retained)}
	}
	if factSources != nil {
		sourceProjection, sourceSelection, sourceCount := factSources.ProjectionDigest, factSources.SelectionDigest, len(factSources.Retained)
		if !factsRetained {
			factSources = newFactProjection("", nil)
		}
		refs := make([]ingressProjectionEvidenceRef, 0, len(factSources.Retained))
		for _, ref := range factSources.Retained {
			refs = append(refs, ingressProjectionEvidenceRef{Type: "memory_projection_item", Ref: "facts:v1:" + factSources.SelectionDigest + ":" + ref.Source.Kind + ":" + ref.ID})
		}
		result["retained_fact_refs"] = refs
		result["facts_projection"] = map[string]any{
			"schema_version": 1, "boundary": "ingress_envelope", "source_projection_digest": sourceProjection,
			"source_selection_digest": sourceSelection, "projection_digest": factSources.ProjectionDigest,
			"selection_digest": factSources.SelectionDigest, "retained_items": factSources.Retained,
			"rendered_bytes": factSources.RenderedBytes, "source_version_state": factSources.SourceVersionState,
			"omitted_count": sourceCount - len(factSources.Retained),
		}
	}
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
			"source_version_state": typed.SourceVersionState,
			"context_accounting":   typed.Accounting,
			"context_sufficiency":  typed.Sufficiency,
		}
		if typed.Recovery != nil {
			result["typed_projection"].(map[string]any)["evidence_recovery"] = typed.Recovery
		}
		if typed.Coverage != nil {
			result["typed_projection"].(map[string]any)["evidence_coverage"] = typed.Coverage
		}
	}
	return result, nil
}

func handleIngressAssembly(state *gatewayState, args commandArgs) ([]byte, bus.ModuleStatus) {
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
	if args.boolean("prepare_source_release") {
		ticket, err := state.releases.prepare(args, result)
		if err != nil {
			return commandResult(commandError("unavailable", "source release preparation unavailable"))
		}
		result["source_release_ticket"] = ticket
		result["exploration_offer"] = state.releases.explorationOffer(ticket, result)
	}
	return commandResult(result)
}

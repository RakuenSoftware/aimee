package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const codeContextMaxItems = 4

type codeContextMemory struct {
	publicMemoryRecord
	Scope Scope `json:"scope"`
}
type codeContextRequest struct {
	Query      string         `json:"query"`
	Symbol     string         `json:"symbol"`
	Project    string         `json:"project"`
	Generation ingressInteger `json:"generation"`
	Hybrid     struct {
		Results           []commandArgs `json:"results"`
		VectorStatus      string        `json:"vector_status"`
		VectorDependency  string        `json:"vector_dependency"`
		RetryAfter        *float64      `json:"vector_retry_after_ms"`
		ObservedDimension *float64      `json:"vector_observed_dimension"`
		CurrentDimension  *float64      `json:"vector_current_dimension"`
	} `json:"hybrid"`
	Enriched []struct {
		Project  string         `json:"project"`
		FilePath string         `json:"file_path"`
		Line     ingressInteger `json:"line"`
	} `json:"enriched"`
	Memories []codeContextMemory `json:"memories"`
}
type codeContextResult struct {
	Status     string `json:"status"`
	HTTPStatus int    `json:"http_status"`
	Packet     any    `json:"packet"`
}
type codeContextAnchor struct {
	Project    string         `json:"project"`
	FilePath   string         `json:"file_path"`
	Generation ingressInteger `json:"generation"`
	Freshness  string         `json:"freshness"`
}

func decodeCodeContext(args commandArgs) (codeContextRequest, bool) {
	var request codeContextRequest
	raw, err := json.Marshal(args)
	if err != nil || json.Unmarshal(raw, &request) != nil || request.Query == "" || request.Project == "" || request.Generation <= 0 || len(request.Enriched) > 25 || len(request.Hybrid.Results) > 100 || len(request.Memories) > 8 {
		return request, false
	}
	return request, true
}

// Only the trusted host supplies code-search results. The production operation
// fetches memory from this same Go owner; caller-supplied memory is never used.
// The plan operation lets native compatibility tests execute the identical
// packet policy with deterministic evidence snapshots.
func handleCodeContext(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	request, ok := decodeCodeContext(args)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if args.stringOr("operation", "") == "code-context-plan" {
		return codeContextCommandResult(buildCodeContext(request))
	}
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	request.Memories = nil
	initial := buildCodeContext(request)
	packet, ok := initial.Packet.(map[string]any)
	if !ok || initial.HTTPStatus != 200 {
		return codeContextCommandResult(initial)
	}
	results := packet["results"].([]map[string]any)
	if len(results) > 0 && len(results) < codeContextMaxItems {
		// Memory is optional explanatory context. A store outage must not discard
		// independently accepted code or leave a failed store transaction open.
		raw, _ := json.Marshal(DataRequest{Operation: "visible-search", Query: request.Query, Project: request.Project, Limit: 8, PublicView: true})
		encoded, status := handleData(options, invocation, raw)
		if status == bus.ModuleStatusOK {
			var response DataResponse
			if json.Unmarshal(encoded, &response) == nil {
				scopes := map[int64]Scope{}
				for _, row := range response.Records {
					scopes[row.ID] = row.Scope
				}
				for _, row := range response.PublicRecords {
					request.Memories = append(request.Memories, codeContextMemory{row, scopes[row.ID]})
				}
			}
		}
	}
	return codeContextCommandResult(buildCodeContext(request))
}

func codeContextClass(row commandArgs) (int, float64, []string) {
	var signals []string
	if json.Unmarshal(row["signals"], &signals) != nil {
		return -1, 0, nil
	}
	code, graph, vector := false, false, false
	for _, signal := range signals {
		switch signal {
		case "code":
			code = true
		case "graph":
			graph = true
		case "vector":
			vector = true
		}
	}
	if code && graph {
		return 0, .95, signals
	}
	if code {
		return 0, .90, signals
	}
	if graph {
		return 0, .85, signals
	}
	if score, ok := row.number("vector_score"); vector && ok && score >= .70 {
		return 1, min(score, 1), signals
	}
	return -1, 0, signals
}

func codeContextTextAnchor(text, path, symbol string) bool {
	if path != "" && strings.Contains(text, path) {
		return true
	}
	if len(symbol) < 3 {
		return false
	}
	identifier := func(c byte) bool {
		return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_'
	}
	for offset := 0; offset < len(text); {
		at := strings.Index(text[offset:], symbol)
		if at < 0 {
			break
		}
		at += offset
		end := at + len(symbol)
		if (at == 0 || !identifier(text[at-1])) && (end == len(text) || !identifier(text[end])) {
			return true
		}
		offset = end
	}
	return false
}

func buildCodeContext(request codeContextRequest) codeContextResult {
	results := []map[string]any{}
	why := []map[string]any{}
	top := 0.0
	for class := 0; class <= 1; class++ {
		for _, row := range request.Hybrid.Results {
			if len(results) == codeContextMaxItems {
				break
			}
			if row.stringOr("project", "") != request.Project {
				continue
			}
			path := row.stringOr("file_path", "")
			cls, confidence, signals := codeContextClass(row)
			if cls != class || path == "" {
				continue
			}
			item := map[string]any{"project": request.Project, "file_path": path, "generation": request.Generation, "freshness": "current", "confidence": confidence, "accepted": true, "provenance": signals}
			for _, key := range []string{"score", "signal_hits", "structural_weight", "snippet", "content_hash", "vector_score"} {
				if raw, ok := row[key]; ok {
					item[key] = raw
				}
			}
			line := ingressInteger(0)
			for _, hit := range request.Enriched {
				if hit.Project == request.Project && hit.FilePath == path {
					line = hit.Line
					break
				}
			}
			if line <= 0 {
				var callerLine ingressInteger
				if json.Unmarshal(row["caller_line"], &callerLine) == nil && callerLine > 0 {
					line = callerLine
				}
			}
			if line < 0 {
				line = 0
			}
			span := map[string]any{"kind": "file", "line_start": line, "line_end": line}
			if line > 0 {
				span["kind"] = "line"
			}
			if symbol := row.stringOr("caller", ""); symbol != "" {
				span["symbol"] = symbol
			}
			item["span"] = span
			results = append(results, item)
			top = max(top, confidence)
		}
	}
	if len(results) > 0 {
		for _, memory := range request.Memories {
			if len(results)+len(why) >= codeContextMaxItems {
				break
			}
			if memory.ID <= 0 || memory.Scope.Type != ScopeProject || memory.Scope.Value != request.Project {
				continue
			}
			switch memory.Kind {
			case "decision", "preference", "constraint", "policy", "requirement":
			default:
				continue
			}
			var anchor map[string]any
			for _, row := range results {
				path := row["file_path"].(string)
				symbol, _ := row["span"].(map[string]any)["symbol"].(string)
				for _, text := range []string{memory.Key, memory.Headline, memory.Content, memory.UseCases} {
					if codeContextTextAnchor(text, path, symbol) {
						anchor = row
						break
					}
				}
				if anchor != nil {
					break
				}
			}
			if anchor == nil {
				continue
			}
			text := memory.Headline
			if text == "" {
				text = memory.Content
			}
			var snippet string
			if raw, ok := anchor["snippet"].(json.RawMessage); ok {
				json.Unmarshal(raw, &snippet)
			}
			if len(text) >= 24 && snippet != "" && (strings.Contains(snippet, text) || strings.Contains(text, snippet)) {
				continue
			}
			item := map[string]any{"memory_id": memory.ID, "kind": memory.Kind, "content": memory.Content, "scope": "project", "scope_rank": 3, "provenance": "memory", "confidence": memory.Confidence,
				"anchor": codeContextAnchor{request.Project, anchor["file_path"].(string), request.Generation, "current"}}
			if memory.Headline != "" {
				item["headline"] = memory.Headline
			}
			why = append(why, item)
		}
	}
	answerable := len(results) > 0
	if !answerable {
		status := request.Hybrid.VectorStatus
		if status == "unavailable" || status == "stale" || status == "unauthorized" {
			dependency := request.Hybrid.VectorDependency
			if dependency == "" {
				dependency = "embedder"
			}
			retryable := status == "unavailable"
			failure := map[string]any{"status": status, "dependency": dependency, "retryable": retryable, "project": request.Project, "generation": request.Generation}
			if retryable {
				retry := 1000
				if request.Hybrid.RetryAfter != nil && *request.Hybrid.RetryAfter > 0 && *request.Hybrid.RetryAfter <= 2147483647 {
					retry = int(*request.Hybrid.RetryAfter)
				}
				failure["retry_after_ms"] = retry
			}
			if request.Hybrid.ObservedDimension != nil {
				failure["observed_dimension"] = *request.Hybrid.ObservedDimension
			}
			if request.Hybrid.CurrentDimension != nil {
				failure["current_dimension"] = *request.Hybrid.CurrentDimension
			}
			http := 503
			if status == "stale" {
				http = 409
			}
			if status == "unauthorized" {
				http = 401
			}
			return codeContextResult{"ok", http, failure}
		}
	}
	status, decision, reason := "abstained", "no_answer", "no_evidence_above_floor"
	if answerable {
		status, decision, reason = "ok", "answerable", "current_project_evidence"
	}
	answerability := map[string]any{"decision": decision, "reason": reason, "candidate_count": len(request.Hybrid.Results), "accepted_code_count": len(results), "top_confidence": top, "vector_floor": .70}
	if !answerable && len(request.Hybrid.Results) > 0 {
		near := []map[string]any{}
		for _, row := range request.Hybrid.Results {
			if len(near) == codeContextMaxItems {
				break
			}
			path := row.stringOr("file_path", "")
			if row.stringOr("project", "") != request.Project || path == "" {
				continue
			}
			entry := map[string]any{"file_path": path}
			if score, ok := row.number("vector_score"); ok {
				entry["vector_score"] = score
			}
			near = append(near, entry)
		}
		answerability["near_misses"] = near
		if len(near) > 0 {
			answerability["hint"] = "no candidate cleared the vector floor; these files were the closest matches and are worth reading directly"
		}
	}
	packet := map[string]any{"query": request.Query, "project": request.Project, "generation": request.Generation, "freshness": "current", "resolved": true, "max_results": 4, "max_tokens": 1200, "status": status, "item_count": len(results) + len(why), "results": results, "why": why, "answerability": answerability}
	if request.Symbol != "" {
		packet["symbol"] = request.Symbol
	}
	return codeContextResult{"ok", 200, packet}
}

// Keep the complete packet as text through native cJSON transport so exact
// int64 generations and memory IDs are never round-tripped through doubles.
func codeContextCommandResult(result codeContextResult) ([]byte, bus.ModuleStatus) {
	raw, err := json.Marshal(result.Packet)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(map[string]any{"status": "ok", "http_status": result.HTTPStatus, "packet_json": string(raw)})
}

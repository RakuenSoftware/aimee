package memory

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const benchmarkReportLimit = 1 << 20

// All benchmark reads use the ordinary owner path, including visible-scope
// fusion. A missing host context must never imply IncludeAll.
func benchmarkRead(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, request DataRequest) (DataResponse, bus.ModuleStatus) {
	request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
	request.IncludeAll = args.boolean("include_all")
	if raw, ok := args["scope"]; ok && json.Unmarshal(raw, &request.Scope) != nil {
		return DataResponse{}, bus.ModuleStatusInvalidRequest
	}
	raw, err := json.Marshal(request)
	if err != nil {
		return DataResponse{}, bus.ModuleStatusInvalidRequest
	}
	encoded, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return DataResponse{}, status
	}
	var response DataResponse
	if json.Unmarshal(encoded, &response) != nil {
		return DataResponse{}, bus.ModuleStatusInternal
	}
	// Get is a visible-row lookup; unlike Search it does not impose an exact
	// primary scope. A benchmark's explicit scope must also constrain expected
	// rows, even when the host has include-all authority or another project.
	if request.Operation == "get" && (request.Scope.Type != "" || request.Scope.Value != "") {
		exact, err := normalizeScope(options.placement, request.Scope)
		if err != nil {
			return DataResponse{}, bus.ModuleStatusInvalidRequest
		}
		visible := response.Records[:0]
		for _, record := range response.Records {
			if record.Scope == exact {
				visible = append(visible, record)
			}
		}
		response.Records = visible
	}
	return response, status
}

type benchmarkCandidate struct {
	ID         int64   `json:"id"`
	Key        string  `json:"key"`
	Content    string  `json:"content"`
	Confidence float64 `json:"confidence"`
	// The old search adapter did not supply salience. Retain its zero-valued
	// field for artifact compatibility, without inventing a ranking signal.
	Salience float64 `json:"salience"`
	Tier     string  `json:"tier"`
	Kind     string  `json:"kind"`
}

type benchmarkHardNegative struct {
	TS          string               `json:"ts"`
	Suite       string               `json:"suite"`
	Task        string               `json:"task"`
	Query       string               `json:"query"`
	Expected    string               `json:"expected"`
	Got         string               `json:"got"`
	Error       string               `json:"error"`
	MemoryError string               `json:"memory_error,omitempty"`
	Candidates  []benchmarkCandidate `json:"top_candidates"`
}

func handleBenchmarkHardNegative(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	var fields [6]string
	for i, name := range []string{"suite", "task", "query", "expected", "got", "error"} {
		var ok bool
		fields[i], ok = args.stringValue(name)
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	artifact := benchmarkHardNegative{TS: time.Now().UTC().Format(time.RFC3339), Suite: fields[0], Task: fields[1], Query: fields[2], Expected: fields[3], Got: fields[4], Error: fields[5], Candidates: []benchmarkCandidate{}}
	if artifact.Query != "" {
		response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "search", Query: artifact.Query, Limit: 5})
		if status == bus.ModuleStatusInvalidRequest {
			return nil, status
		}
		if status != bus.ModuleStatusOK {
			// Retrieval failure is part of the artifact, never an empty successful search.
			artifact.MemoryError = "memory retrieval index unavailable"
		} else {
			for _, r := range response.Records[:min(5, len(response.Records))] {
				artifact.Candidates = append(artifact.Candidates, benchmarkCandidate{ID: r.ID, Key: r.Key, Content: r.Content, Confidence: r.Confidence, Tier: r.Tier, Kind: r.Kind})
			}
		}
	}
	line, err := json.Marshal(artifact)
	if err != nil || len(line) > benchmarkReportLimit {
		return nil, bus.ModuleStatusInternal
	}
	// Transport a string: native JSON libraries must not round numeric IDs by
	// decoding and re-serializing the artifact.
	return commandResult(map[string]any{"status": "ok", "line": string(line)})
}

func benchmarkFailureBucket(query string, expected *Diagnostic) string {
	if expected == nil {
		return "missing"
	}
	temporal := false
	for _, term := range []string{"when", "date", "before", "after", "year", "month", "today", "yesterday"} {
		temporal = temporal || strings.Contains(query, term)
	}
	if temporal && expected.Parts.Temporal <= 0 {
		for _, term := range []string{"20", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"} {
			if strings.Contains(expected.Memory.Content, term) {
				return "temporal_miss"
			}
		}
	}
	p := expected.Parts
	switch {
	case p.Entity <= 0:
		return "entity_miss"
	case p.Lexical <= 0 && p.Semantic > 0:
		return "lexical_gap"
	case p.Semantic <= 0 && p.Lexical < 1.5:
		return "semantic_gap"
	case p.State < -.1:
		return "state_penalty"
	case p.Coverage < .5:
		return "granularity_gap"
	default:
		return "ranking_gap"
	}
}

type benchmarkMiss struct {
	Status string `json:"status"`
	Rank   int    `json:"rank"`
	Miss   bool   `json:"is_miss"`
	Bucket string `json:"bucket"`
	Text   string `json:"text"`
}

func handleBenchmarkMiss(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, ok := args.stringValue("query")
	var ids []string
	if !ok || json.Unmarshal(args["expected_ids"], &ids) != nil || ids == nil || len(ids) > 20 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	expectedIDs := make([]int64, len(ids))
	for i, id := range ids {
		n, err := strconv.ParseInt(id, 10, 64)
		if err != nil || n <= 0 || strconv.FormatInt(n, 10) != id {
			return nil, bus.ModuleStatusInvalidRequest
		}
		expectedIDs[i] = n
	}
	response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "search", Query: query, Limit: 20})
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	result := benchmarkMiss{Status: "ok", Bucket: "hit"}
	for i, row := range response.Records {
		for _, id := range expectedIDs {
			if row.ID == id {
				result.Rank = i + 1
				break
			}
		}
		if result.Rank > 0 {
			break
		}
	}
	result.Miss = result.Rank == 0 || result.Rank > args.integer("limit", 10)
	if !result.Miss {
		return commandResult(result)
	}
	var expected *Diagnostic
	if len(expectedIDs) > 0 {
		// Get distinguishes a hidden/absent row from a failed read. Use the same
		// diagnostic computation as Explain, without the public double-ID adapter.
		record, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "get", ID: expectedIDs[0]})
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		if len(record.Records) == 1 {
			diagnostic := diagnosticFor(record.Records[0], query)
			expected = &diagnostic
		}
	}
	result.Bucket = benchmarkFailureBucket(query, expected)
	if args.boolean("render") {
		var text strings.Builder
		rank := "missing"
		if result.Rank > 0 {
			rank = strconv.Itoa(result.Rank)
		}
		fmt.Fprintf(&text, "MISS rank=%s bucket=%s query=%s\n", rank, result.Bucket, query)
		if expected != nil {
			fmt.Fprintf(&text, "  expected_id=%d key=%s\n  expected_content=%s\n", expected.Memory.ID, expected.Memory.Key, expected.Memory.Content)
		}
		if len(response.Records) > 0 {
			text.WriteString("  top_results:")
			for i, row := range response.Records[:min(5, len(response.Records))] {
				fmt.Fprintf(&text, " [%d]%d:%s", i+1, row.ID, row.Key)
			}
			text.WriteByte('\n')
		}
		result.Text = text.String()
		if len(result.Text) > benchmarkReportLimit {
			return nil, bus.ModuleStatusInternal
		}
	}
	return commandResult(result)
}

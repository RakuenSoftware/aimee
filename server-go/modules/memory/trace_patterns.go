package memory

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Trace collection and its durable cursor are separate from pattern detection.
// This host-only operation accepts one ordered batch; it does not acknowledge
// consumption or grant permission to write its findings into another placement.
type traceObservation struct {
	PlanID int64  `json:"plan_id"`
	Tool   string `json:"tool_name"`
	Result string `json:"tool_result"`
}

type tracePattern struct {
	Type       string  `json:"type"`
	Key        string  `json:"key"`
	Content    string  `json:"content"`
	Confidence float64 `json:"confidence"`
}

func traceResultError(result string) bool {
	// Keep the existing detector's literal indicators. This is a heuristic, not
	// a claim that every tool result has a structured success/error envelope.
	for _, indicator := range []string{"error", "Error", "ERROR", "failed", "Failed", "FAILED", "No such file", "not found", "Permission denied", "command not found"} {
		if strings.Contains(result, indicator) {
			return true
		}
	}
	return false
}

func tracePatterns(rows []traceObservation) ([]tracePattern, error) {
	patterns := []tracePattern{}
	seen := map[string]bool{}
	add := func(kind, key, content string, confidence float64) error {
		identity := kind + ":" + key
		if seen[identity] {
			return nil
		}
		// Generated content still crosses the canonical memory write boundary.
		// Refuse a sensitive key and screen its content before transmission.
		clean, err := screenMemoryWrite(key, content)
		if err != nil {
			return err
		}
		seen[identity] = true
		patterns = append(patterns, tracePattern{kind, key, clean, confidence})
		return nil
	}
	for i := 0; i < len(rows); i++ {
		if rows[i].Tool == "" {
			continue
		}
		run, failures := 1, 0
		if traceResultError(rows[i].Result) {
			failures++
		}
		for j := i + 1; j < len(rows) && rows[j].PlanID == rows[i].PlanID && rows[j].Tool == rows[i].Tool; j++ {
			run++
			if traceResultError(rows[j].Result) {
				failures++
			}
		}
		if run >= 3 && failures >= 2 {
			key := fmt.Sprintf("Retry loop: %s called %d times with %d errors", rows[i].Tool, run, failures)
			content := fmt.Sprintf("Tool '%s' was called %d consecutive times with %d failures. Consider a different approach after 2 failures.", rows[i].Tool, run, failures)
			if err := add("anti-pattern", key, content, .7); err != nil {
				return nil, err
			}
			i += run - 1
		}
	}
	for i := 0; i+1 < len(rows); i++ {
		a, b := rows[i], rows[i+1]
		if a.PlanID != b.PlanID || a.Tool == "" || b.Tool == "" || a.Tool == b.Tool || !traceResultError(a.Result) || traceResultError(b.Result) {
			continue
		}
		if err := add("procedure", "recovery:"+a.Tool+"->"+b.Tool, fmt.Sprintf("When '%s' fails, try '%s' as a recovery step.", a.Tool, b.Tool), .7); err != nil {
			return nil, err
		}
	}
	plans := map[int64]bool{}
	for _, row := range rows {
		if row.PlanID != 0 {
			plans[row.PlanID] = true
		}
	}
	if len(plans) < 3 {
		return patterns, nil
	}
	type pair struct{ a, b string }
	order := []pair{}
	pairs := map[pair]map[int64]bool{}
	for i := 0; i+1 < len(rows); i++ {
		a, b := rows[i], rows[i+1]
		if a.PlanID == 0 || a.PlanID != b.PlanID || a.Tool == "" || b.Tool == "" {
			continue
		}
		p := pair{a.Tool, b.Tool}
		if pairs[p] == nil {
			pairs[p] = map[int64]bool{}
			order = append(order, p)
		}
		pairs[p][a.PlanID] = true
	}
	for _, p := range order {
		ratio := float64(len(pairs[p])) / float64(len(plans))
		if ratio < .6 {
			continue
		}
		content := fmt.Sprintf("Common pattern: '%s' is typically followed by '%s' (observed in %d/%d plans, %.0f%%).", p.a, p.b, len(pairs[p]), len(plans), ratio*100)
		if err := add("procedure", "sequence:"+p.a+"->"+p.b, content, .6+ratio*.2); err != nil {
			return nil, err
		}
	}
	return patterns, nil
}

func handleTracePatterns(args commandArgs) ([]byte, bus.ModuleStatus) {
	var rows []traceObservation
	if len(args["traces"]) > 1<<20 || json.Unmarshal(args["traces"], &rows) != nil || rows == nil || len(rows) > 512 {
		return commandResult(commandError("invalid_argument", "trace pattern analysis requires at most 512 observations within one MiB"))
	}
	for _, row := range rows {
		if row.PlanID < 0 || len(row.Tool) > 1024 || strings.ContainsRune(row.Tool, '\x00') {
			return commandResult(commandError("invalid_argument", "invalid trace plan or tool name"))
		}
	}
	patterns, err := tracePatterns(rows)
	if err != nil {
		return commandResult(commandError("invalid_argument", "sensitive trace pattern refused"))
	}
	encoded, status := commandResult(map[string]any{"status": "ok", "patterns": patterns})
	if len(encoded) > 1<<20 {
		return commandResult(commandError("capacity_exceeded", "trace findings exceed one MiB"))
	}
	return encoded, status
}

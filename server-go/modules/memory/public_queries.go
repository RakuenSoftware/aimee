package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func (args commandArgs) integer(name string, fallback int) int {
	value, ok := args.number(name)
	if !ok {
		return fallback
	}
	return int(math.Max(math.Min(value, math.MaxInt32), math.MinInt32))
}

func handleQueryCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if verb == "list_unused_l2" && args.stringOr("view", "") == "stale" {
		return handleStaleInspection(options, invocation, args)
	}
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "key_exists", "find_id_by_key_kind":
		request.Key = args.stringOr("key", "")
		if request.Key == "" {
			return invalid("missing key")
		}
		request.Operation = "key-exists"
		scoped = commandScope(args, &request)
		if verb == "find_id_by_key_kind" {
			var ok bool
			request.Kind, ok = args.stringValue("kind")
			if !ok {
				return invalid("memory.find_id_by_key_kind requires key and kind")
			}
			request.Operation = "find-id"
			scoped = commandScope(args, &request)
		}
	case "list_low_effectiveness":
		threshold := 0.5
		if args.stringOr("view", "") == "console" {
			threshold = 0.3
		}
		if n, ok := args.number("threshold"); ok {
			threshold = n
		}
		if threshold < 0 || threshold > 1 {
			return invalid("threshold must be between zero and one")
		}
		request.Operation, request.Confidence, request.Limit = "low-effectiveness", &threshold, args.limit("limit", 50, 256)
		if args.stringOr("view", "") == "console" && args.integer("limit", 50) <= 0 {
			request.Limit = 256
		}
	case "list_unused_l2":
		request.Operation, request.Days, request.Limit = "unused-l2", args.integer("days", 14), args.limit("max", 64, 256)
	case "list_superseded_keys":
		request.Operation, request.MinVersions, request.Limit = "superseded-keys", args.integer("min_versions", 3), args.limit("max", 64, 256)
	case "review_list", "review_console":
		request.Operation, request.State, request.Limit = "review-list", args.stringOr("state", ""), args.limit("limit", 64, 64)
		scoped = commandScope(args, &request)
		if verb == "review_console" {
			request.State, request.Limit, request.IncludeAll, scoped = "", 32, true, false
		}
	case "set_artifact":
		var idOK, typeOK, refOK bool
		request.ID, idOK = args.positiveID("memory_id")
		request.ArtifactType, typeOK = args.stringValue("artifact_type")
		request.ArtifactRef, refOK = args.stringValue("artifact_ref")
		if !idOK || !typeOK || !refOK {
			return invalid("memory.set_artifact requires memory_id, artifact_type, artifact_ref")
		}
		request.Operation, request.ArtifactHash = "set-artifact", args.stringOr("artifact_hash", "")
	case "effectiveness_stats":
		request.Operation = "effectiveness-stats"
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok"}
	switch verb {
	case "key_exists":
		if response.Allowed == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["exists"] = *response.Allowed
	case "find_id_by_key_kind":
		if len(response.IDs) != 1 {
			return nil, bus.ModuleStatusInternal
		}
		result["id"] = response.IDs[0]
		if args.stringOr("view", "") == "native" {
			result["id_text"] = fmt.Sprint(response.IDs[0])
		}
	case "list_low_effectiveness":
		rows := response.LowEffectiveness
		if rows == nil {
			rows = []LowEffectiveness{}
		}
		if args.stringOr("view", "") == "console" {
			return inspectionOutput(rows, "", args)
		}
		result["rows"] = rows
	case "list_unused_l2":
		rows := make([]map[string]any, 0, len(response.Records))
		for _, r := range response.Records {
			rows = append(rows, map[string]any{"id": r.ID, "key": r.Key, "tier": r.Tier, "kind": r.Kind, "confidence": r.Confidence})
		}
		result["rows"] = rows
	case "list_superseded_keys":
		rows := response.SupersededKeys
		if rows == nil {
			rows = []SupersededKey{}
		}
		result["rows"] = rows
	case "review_list", "review_console":
		rows := make([]map[string]any, 0, len(response.Reviews))
		for _, r := range response.Reviews {
			rows = append(rows, map[string]any{"id": r.ID, "tier": r.Tier, "kind": r.Kind, "key": r.Key, "content": r.Content, "confidence": r.Confidence, "lifecycle": r.LifecycleState, "review_reason": r.ReviewReason, "scope_type": r.ScopeType, "scope_value": r.ScopeValue, "created_at": r.CreatedAt, "updated_at": r.UpdatedAt})
		}
		result["memories"] = rows
		if verb == "review_console" {
			result["schema"], result["count"] = "console.memories.v1", len(rows)
		}
	case "set_artifact":
		if !response.Updated {
			return commandResult(commandError("not_found", "memory not found"))
		}
	case "effectiveness_stats":
		s := response.Effectiveness
		if s == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["stats"] = map[string]any{"avg_effectiveness": s.Average, "low_effectiveness_count": s.LowCount, "high_impact_count": s.HighImpactCount, "never_surfaced_l2": s.NeverSurfacedL2}
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

func inspectionOutput(payload any, text string, args commandArgs) ([]byte, bus.ModuleStatus) {
	format := args.stringOr("format", "json")
	if format == "text" {
		return commandResult(map[string]any{"status": "ok", "output": text})
	}
	if format != "json" {
		return commandResult(commandError("invalid_argument", "format must be json or text"))
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	output, err := memoryJSONOutput(raw, args)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(map[string]any{"status": "ok", "output": output + "\n"})
}

// The stale provenance console is a composite read. Both queries must succeed
// before emitting any output; an unavailable half is not an empty result.
func handleStaleInspection(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	var unused, superseded DataResponse
	for i, raw := range []string{`{"operation":"unused-l2","days":14,"limit":256,"include_all":true}`, `{"operation":"superseded-keys","min_versions":3,"limit":256,"include_all":true}`} {
		data, status := handleData(options, invocation, []byte(raw))
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		out := &unused
		if i == 1 {
			out = &superseded
		}
		if json.Unmarshal(data, out) != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	rows := make([]map[string]any, 0, len(unused.Records))
	var text strings.Builder
	text.WriteString("Never-used L2 memories (>14 days old):\n")
	for _, row := range unused.Records {
		rows = append(rows, map[string]any{"id": row.ID, "key": row.Key})
		fmt.Fprintf(&text, "  #%-6d %s\n", row.ID, row.Key)
	}
	if len(rows) == 0 {
		text.WriteString("  (none)\n")
	}
	text.WriteString("\nFrequently superseded keys (3+ versions):\n")
	if superseded.SupersededKeys == nil {
		superseded.SupersededKeys = []SupersededKey{}
	}
	for _, row := range superseded.SupersededKeys {
		fmt.Fprintf(&text, "  %-40s %d versions\n", row.BaseKey, row.Versions)
	}
	if len(superseded.SupersededKeys) == 0 {
		text.WriteString("  (none)\n")
	}
	return inspectionOutput(map[string]any{"never_used": rows, "frequently_superseded": superseded.SupersededKeys}, text.String(), args)
}

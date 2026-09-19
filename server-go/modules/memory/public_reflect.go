package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleReflectCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, ok := args.stringValue("query")
	format := args.stringOr("format", "")
	if !ok || strings.TrimSpace(query) == "" || len(query) > 2047 || (format != "" && format != "json" && format != "text") {
		return commandResult(commandError("invalid_argument", "reflection requires query (up to 2047 bytes) and optional json/text format"))
	}
	limit := args.integer("limit", 10)
	if limit < 1 || limit > 32 {
		limit = 10
	}
	request := DataRequest{Operation: "reflect", Query: query, Limit: limit, Reflection: &reflectionOptions{DraftRule: args.boolean("draft_rule"), Synthesize: args.boolean("synthesize")}}
	if !commandScope(args, &request) {
		// Reflection defaults to visible/shared memory; an omitted context is
		// never permission to enumerate every private project.
		request.IncludeAll = false
		request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
	}
	request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
	if request.Scope.Type != "" || request.Scope.Value != "" {
		request.IncludeAll = false
		if _, err := normalizeScope(PlacementKB, request.Scope); err != nil {
			return commandResult(commandError("invalid_argument", err.Error()))
		}
	}
	raw, _ := json.Marshal(request)
	encoded, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory reflection unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	var result reflectionResult
	if json.Unmarshal(encoded, &response) != nil || json.Unmarshal(response.Payload, &result) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if format == "text" {
		return commandResult(map[string]any{"status": "ok", "output": reflectionText(result)})
	}
	if format == "json" {
		// Filter only the top-level CLI fields, retaining exact integer JSON bytes.
		// The compact profile strips no fields present in a reflection result.
		raw, _ = json.Marshal(result)
		if fields, exists := args.stringValue("fields"); exists {
			var values map[string]json.RawMessage
			_ = json.Unmarshal(raw, &values)
			filtered := map[string]json.RawMessage{}
			for _, field := range strings.Split(fields, ",") {
				field = strings.TrimSpace(field)
				if value, found := values[field]; found {
					filtered[field] = value
				}
			}
			raw, _ = json.Marshal(filtered)
		}
		return commandResult(map[string]any{"status": "ok", "output": string(raw)})
	}
	return commandResult(struct {
		Status string `json:"status"`
		reflectionResult
	}{"ok", result})
}

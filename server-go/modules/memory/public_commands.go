package memory

import (
	"context"
	"encoding/json"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventCommand uint32 = 5896
	StageCommand uint32 = 8
)

// Command invokes the public command contract. Its result is the complete
// surface response, including typed argument and not-found errors. Transport
// failures remain errors and are never retried here.
func (c *Client) Command(ctx context.Context, trace uint64, verb string, args json.RawMessage) (json.RawMessage, error) {
	frame, err := bus.EncodeCommand(verb, args)
	if err != nil {
		return nil, ErrClientRequest
	}
	reply, err := c.call(ctx, EventCommand, StageCommand, trace, frame)
	if err != nil {
		return nil, err
	}
	body, err := bus.DecodeCommandResult(reply)
	if err != nil {
		return nil, ErrClientResponse
	}
	var object map[string]json.RawMessage
	if json.Unmarshal(body, &object) != nil || object == nil {
		return nil, ErrClientResponse
	}
	var status string
	if json.Unmarshal(object["status"], &status) != nil || (status != "ok" && status != "error") {
		return nil, ErrClientResponse
	}
	return body, nil
}

type commandArgs map[string]json.RawMessage

func (args commandArgs) stringOr(name, fallback string) string {
	var value string
	if raw, ok := args[name]; ok && string(raw) != "null" && json.Unmarshal(raw, &value) == nil {
		return value
	}
	return fallback
}

func (args commandArgs) number(name string) (float64, bool) {
	raw, ok := args[name]
	if !ok || string(raw) == "null" {
		return 0, false
	}
	var value float64
	if json.Unmarshal(raw, &value) != nil || math.IsNaN(value) || math.IsInf(value, 0) {
		return 0, false
	}
	return value, true
}

func (args commandArgs) positiveID(name string) (int64, bool) {
	value, ok := args.number(name)
	if !ok || value <= 0 || value > 9007199254740991 || math.Trunc(value) != value {
		return 0, false
	}
	return int64(value), true
}

func commandError(kind, message string) map[string]any {
	return map[string]any{"status": "error", "kind": kind, "message": message}
}

func commandResult(value any) ([]byte, bus.ModuleStatus) {
	body, err := json.Marshal(value)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	frame, err := bus.EncodeCommandResult(body)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return frame, bus.ModuleStatusOK
}

// The appliance's private commands use the same data owner as stage 7. Public
// callers cannot submit data-stage operations or widen the owner's scope by
// including project, workspace, authority, or scope fields in their arguments.
// Shared-KB dispatch remains at the server boundary until its client is ported.
func handleCommand(options handlerOptions, invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
	verb, body, err := bus.DecodeCommand(frame)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var args commandArgs
	if json.Unmarshal(body, &args) != nil || args == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if options.placement != PlacementServer {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	if _, exists := args["store"]; exists && args.stringOr("store", "") != "user" {
		return invalid("local memory commands require store=user")
	}
	request := DataRequest{Operation: verb, Scope: Scope{Type: ScopeUser}}
	confidence := 1.0
	switch verb {
	case "get", "delete":
		var ok bool
		request.ID, ok = args.positiveID("id")
		if !ok {
			return invalid("memory." + verb + " requires a positive integer id")
		}
		if _, exists := args["as_of"]; verb == "get" && exists {
			return invalid("historical reads require store=kb")
		}
	case "store", "supersede":
		if _, exists := args["confidence"]; exists {
			var ok bool
			confidence, ok = args.number("confidence")
			if !ok || confidence < 0 || confidence > 1 {
				return invalid("memory." + verb + " confidence must be between 0 and 1")
			}
		}
		request.Confidence = &confidence
		if verb == "store" {
			if args.stringOr("key", "") == "" || args.stringOr("content", "") == "" {
				var key, content string
				if json.Unmarshal(args["key"], &key) != nil || string(args["key"]) == "null" ||
					json.Unmarshal(args["content"], &content) != nil || string(args["content"]) == "null" {
					return invalid("memory.store requires a key and content")
				}
				return invalid("memory.store requires a non-empty key and content")
			}
			request.Key, request.Content = args.stringOr("key", ""), args.stringOr("content", "")
			request.Tier, request.Kind = args.stringOr("tier", "L2"), args.stringOr("kind", "fact")
		} else {
			var ok bool
			request.ID, ok = args.positiveID("old_id")
			if !ok {
				return invalid("memory.supersede requires a positive integer old_id")
			}
			request.Content = args.stringOr("new_content", "")
			if request.Content == "" {
				return invalid("memory.supersede requires non-empty new_content")
			}
			if raw, exists := args["session_id"]; exists {
				var session string
				if string(raw) == "null" || json.Unmarshal(raw, &session) != nil {
					return invalid("memory.supersede session_id must be a string")
				}
			}
		}
	case "list":
		request.Tier, request.Kind = args.stringOr("tier", ""), args.stringOr("kind", "")
		request.Limit = 20
		if value, ok := args.number("limit"); ok {
			// Preserve the old cJSON integer conversion before data-stage bounds.
			request.Limit = int(math.Max(math.Min(value, math.MaxInt32), math.MinInt32))
		}
	case "search":
		request.Limit = 10
		if _, exists := args["limit"]; exists {
			value, ok := args.number("limit")
			if !ok || value < 1 || value > 32 || math.Trunc(value) != value {
				return invalid("memory.search limit must be an integer between 1 and 32")
			}
			request.Limit = int(value)
		}
		var keywords []json.RawMessage
		if json.Unmarshal(args["keywords"], &keywords) != nil || len(keywords) == 0 {
			return invalid("missing or empty keywords array")
		}
		if len(keywords) > 16 {
			return invalid("memory.search accepts at most 16 keywords")
		}
		terms := make([]string, len(keywords))
		for i, raw := range keywords {
			if json.Unmarshal(raw, &terms[i]) != nil || terms[i] == "" {
				return invalid("memory.search keywords must be non-empty strings")
			}
		}
		request.Query = strings.Join(terms, " ")
		if len(request.Query) > 2047 {
			request.Query = request.Query[:2047]
		}
	case "stats":
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status == bus.ModuleStatusCancelled || status == bus.ModuleStatusDeadlineExceeded {
		return nil, status
	}
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "user memory module unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok", "store": "user"}
	switch verb {
	case "store", "get", "supersede":
		if len(response.Records) == 0 {
			if verb == "store" {
				return commandResult(commandError("unavailable", "user memory module unavailable"))
			}
			return commandResult(commandError("not_found", "user memory not found"))
		}
		record := response.Records[0]
		switch verb {
		case "store":
			result["id"] = record.ID
		case "get":
			result["memory"] = record
		case "supersede":
			// Supersede's established envelope contains the record at the root.
			encoded, _ := json.Marshal(record)
			if json.Unmarshal(encoded, &result) != nil {
				return nil, bus.ModuleStatusInternal
			}
		}
	case "delete":
		if !response.Deleted {
			return commandResult(commandError("not_found", "no such user memory, or the memory module refused"))
		}
		result["id"], result["deleted"], result["destroyed"] = request.ID, true, false
	case "list", "search":
		if response.Records == nil {
			return commandResult(commandError("unavailable", "user memory module unavailable"))
		}
		result["active_context_missing"] = false
		if verb == "list" {
			result["memories"] = response.Records
		} else {
			delete(result, "store") // Preserve the search envelope consumed by CLI/MCP.
			result["facts"], result["windows"] = response.Records, []any{}
		}
	case "stats":
		if response.Stats == nil {
			return commandResult(commandError("unavailable", "user memory module unavailable"))
		}
		result["stats"] = response.Stats
	}
	return commandResult(result)
}

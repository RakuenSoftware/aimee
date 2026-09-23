package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventCommand uint32 = 5896
	StageCommand uint32 = 8
)

// Command invokes the public command contract. Its result is the complete
// surface response, including degraded evidence, typed argument and not-found errors. Transport
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
	if json.Unmarshal(object["status"], &status) != nil || (status != "ok" && status != "error" && status != "degraded" && !(verb == "benchmark" && status == "async-only")) {
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
	verb, body, caller, err := bus.DecodeCommandWithContext(frame)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if caller != nil && invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	options.commandContext = caller
	var args commandArgs
	if json.Unmarshal(body, &args) != nil || args == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, exists := args["read_policy"]; exists && verb != "get" && verb != "runtime" {
		return commandResult(commandError("unsupported_mode", "read_policy is supported only for exact-ID get"))
	}
	if _, exists := args["idempotency_key"]; exists && !((options.placement == PlacementKB && (verb == "store" || verb == "supersede" || verb == "update" || verb == "delete" || verb == "reject" || verb == "restore")) || (options.placement == PlacementServer && (verb == "store" || verb == "supersede" || verb == "delete" || verb == "runtime"))) {
		return commandResult(commandError("unsupported_mode", "idempotency_key is supported for store, conditional corrections, deletion and shared reject/restore"))
	}
	versionedMutation := (options.placement == PlacementKB && (verb == "supersede" || verb == "update" || verb == "delete" || verb == "reject" || verb == "restore" || verb == "review_correction")) || (options.placement == PlacementServer && (verb == "supersede" || verb == "delete" || verb == "runtime"))
	if _, exists := args["expected_version"]; exists && !versionedMutation {
		return commandResult(commandError("unsupported_mode", "expected_version is supported for supersede, shared update/reject/restore, deletion and correction review"))
	}
	if _, exists := args["include_version"]; exists && verb != "get" && !(verb == "runtime" && options.placement == PlacementServer) {
		return commandResult(commandError("unsupported_mode", "include_version is supported only for exact-ID get"))
	}
	if _, exists := args["at_version"]; exists && (options.placement != PlacementServer || (verb != "get" && verb != "runtime")) {
		return commandResult(commandError("unsupported_mode", "at_version is supported only for personal exact-ID get"))
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if verb == "runtime" {
		return handleRuntimeView(options, invocation, args)
	}
	if verb == "embed_text" {
		if invocation.PrincipalRef != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		encoded, status := handleEmbed(options.executor, options, invocation, body)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		var response map[string]any
		if json.Unmarshal(encoded, &response) != nil {
			return nil, bus.ModuleStatusInternal
		}
		if response["truncated"] == true {
			delete(response, "vector")
			response["dim"] = 0
			response["error"] = "embed: vector exceeds requested dimension"
		}
		if args.stringOr("operation", "") == "serving-id" && response["error"] == nil && response["serving_id"] == nil {
			response["serving_id"] = ""
		}
		return commandResult(response)
	}
	for _, route := range sharedCommandRoutes {
		if route.verb == verb {
			return route.handler(options, invocation, verb, args)
		}
	}
	if options.placement == PlacementKB {
		for _, route := range kbCommandRoutes {
			if route.verb == verb {
				return route.handler(options, invocation, verb, args)
			}
		}
	}
	return handleUserCommand(options, invocation, verb, args)
}

func handleUserCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
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
	if caller := options.commandContext; caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
		request.Authority = AuthorityUser
	}

	if _, exists := args["idempotency_key"]; exists && verb != "store" && verb != "supersede" && verb != "delete" {
		return commandResult(commandError("unsupported_mode", "private idempotency keys require store, conditional supersede or delete"))
	}
	confidence := 1.0
	switch verb {
	case "get", "delete":
		var ok bool
		if verb == "get" {
			request.ReadPolicy, ok = commandReadPolicy(args)
			if !ok {
				return invalid("read_policy must be a versioned object with recognized fields")
			}
		}
		request.ID, ok = args.decimalID("id")
		if !ok {
			return invalid("memory." + verb + " requires a positive integer id")
		}
		if verb == "delete" {
			var refusal map[string]any
			request.ExpectedVersion, request.IdempotencyKey, refusal = commandCorrectionOptions(args, request.ID, options.commandContext)
			if refusal != nil {
				return commandResult(refusal)
			}
		}
		if verb == "get" {
			if raw, exists := args["include_version"]; exists && (string(raw) == "null" || json.Unmarshal(raw, &request.IncludeVersion) != nil) {
				return invalid("include_version must be boolean")
			}
			request.AtVersion, ok = commandRecordVersion(args, "at_version", request.ID)
			if !ok || (request.AtVersion != nil && request.ReadPolicy != nil) {
				return invalid("at_version requires a valid record version and cannot be combined with read_policy")
			}
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
			var refusal map[string]any
			request.IdempotencyKey, refusal = commandCreationKey(args, options.commandContext)
			if refusal != nil {
				return commandResult(refusal)
			}
		} else {
			var ok bool
			request.ID, ok = args.decimalID("old_id")
			if !ok {
				return invalid("memory.supersede requires a positive integer old_id")
			}
			var refusal map[string]any
			request.ExpectedVersion, request.IdempotencyKey, refusal = commandCorrectionOptions(args, request.ID, options.commandContext)
			if refusal != nil {
				return commandResult(refusal)
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
				request.SessionID = session
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
		terms, limit, err := serverSearchArguments(args)
		if err != nil {
			return invalid(err.Error())
		}
		request.Query, request.Limit = strings.Join(terms, " "), limit
	case "review-list":
		request.State, request.Limit = args.stringOr("state", ""), 64
		if value, ok := args.number("limit"); ok {
			request.Limit = int(math.Max(math.Min(value, math.MaxInt32), math.MinInt32))
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
	if response.Read != nil && response.Read.ErrorCode != "" {
		return commandResult(commandError(response.Read.ErrorCode, response.Read.Message))
	}
	if refusal := commandMutationRefusal(response.Code, response.Proposal); refusal != nil {
		return commandResult(refusal)
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
			if response.MutationReceipt != nil {
				result["mutation_receipt"] = response.MutationReceipt
			}
		case "get":
			result["memory"] = record
			if response.Read != nil {
				result["read"] = response.Read
			}
		case "supersede":
			if args.stringOr("view", "") == "mcp" {
				result["records"] = response.Records
				if response.MutationReceipt != nil {
					result["mutation_receipt"] = response.MutationReceipt
				}
				return commandResult(result)
			}
			// Supersede's established envelope contains the record at the root.
			return commandResult(struct {
				Record
				Status          string                 `json:"status"`
				Store           string                 `json:"store"`
				MutationReceipt *MemoryMutationReceipt `json:"mutation_receipt,omitempty"`
			}{record, "ok", "user", response.MutationReceipt})
		}
	case "delete":
		if !response.Deleted {
			return commandResult(commandError("not_found", "no such user memory, or the memory module refused"))
		}
		result["id"], result["deleted"], result["destroyed"] = request.ID, true, false
		if response.MutationReceipt != nil {
			result["mutation_receipt"] = response.MutationReceipt
		}
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
	case "review-list":
		rows := make([]struct {
			ReviewRecord
			Lifecycle string `json:"lifecycle"`
		}, 0, len(response.Reviews))
		for _, row := range response.Reviews {
			rows = append(rows, struct {
				ReviewRecord
				Lifecycle string `json:"lifecycle"`
			}{row, row.LifecycleState})
		}
		result["memories"] = rows
	}
	return commandResult(result)
}

// Recall accepts a bounded activation snapshot from the authenticated appliance.
// Scope fields are decoded here rather than in the native KB request adapter.
func handleRecallCommand(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: "recall-bundle", Query: args.stringOr("task_hint", ""), Activation: args["activation"], IncludeAll: true}
	if value, ok := args.number("limit_tokens"); ok {
		request.LimitTokens = int(math.Max(math.Min(value, math.MaxInt32), math.MinInt32))
	}
	_ = json.Unmarshal(args["session_start"], &request.SessionStart)
	request.LimitTokens = recallTokenLimit(request.LimitTokens, request.SessionStart)
	var scoped bool
	_ = json.Unmarshal(args["scope_context"], &scoped)
	if scoped {
		request.Workspace, request.Project = args.stringOr("workspace", ""), args.stringOr("project", "")
		request.IncludeAll = false
		_ = json.Unmarshal(args["include_all"], &request.IncludeAll)
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusCapabilityAbsent || status == bus.ModuleStatusInternal {
			return commandResult(commandError("unavailable", "memory recall unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	var outcome struct {
		Status string `json:"status"`
	}
	if json.Unmarshal(response.Payload, &outcome) == nil && outcome.Status == "error" {
		return commandResult(response.Payload)
	}
	result := map[string]any{"status": "ok", "recall": response.Payload}
	if options.placement == PlacementServer {
		result["store"] = "user"
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

// Both placements validate the same Server contract and data-stage query bound.
// Silently truncating input could change query meaning or UTF-8.
func serverSearchArguments(args commandArgs) ([]string, int, error) {
	limit := 10
	if _, exists := args["limit"]; exists {
		value, ok := args.number("limit")
		if !ok || value < 1 || value > 32 || math.Trunc(value) != value {
			return nil, 0, errors.New("memory.search limit must be an integer between 1 and 32")
		}
		limit = int(value)
	}
	var keywords []json.RawMessage
	if json.Unmarshal(args["keywords"], &keywords) != nil || len(keywords) == 0 {
		return nil, 0, errors.New("missing or empty keywords array")
	}
	if len(keywords) > 16 {
		return nil, 0, errors.New("memory.search accepts at most 16 keywords")
	}
	terms := make([]string, len(keywords))
	for i, raw := range keywords {
		if json.Unmarshal(raw, &terms[i]) != nil || strings.TrimSpace(terms[i]) == "" {
			return nil, 0, errors.New("memory.search keywords must be non-empty strings")
		}
	}
	if len(strings.Join(terms, " ")) > 16384 {
		return nil, 0, errors.New("memory.search query exceeds 16384 bytes")
	}
	return terms, limit, nil
}

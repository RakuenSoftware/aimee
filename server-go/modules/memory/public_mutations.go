package memory

import (
	"encoding/json"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
)

func commandMutationRefusal(code *int) map[string]any {
	if code == nil {
		return nil
	}
	switch *code {
	case MutationIdempotencyConflict:
		result := commandError("conflict", errIdempotencyConflict.Error())
		result["reason"] = "idempotency_conflict"
		return result
	case MutationReplayUnavailable:
		result := commandError("conflict", errReplayUnavailable.Error())
		result["reason"] = "idempotent_result_unavailable"
		return result
	case MutationVersionConflict:
		result := commandError("conflict", errMutationVersionConflict.Error())
		result["reason"] = "expected_version_conflict"
		return result
	case MutationImmutableExperience:
		return commandError("conflict", errImmutableExperience.Error())
	case MutationRequiresReplacement:
		return commandError("conflict", errRequiresRevocation.Error())
	case MutationReviewRequired:
		return commandError("review_required", errMutationReviewRequired.Error())
	}
	return nil
}

func handleMutationCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	if args.stringOr("view", "") == "mcp" && verb != "delete" && verb != "update" && verb != "touch" && verb != "reject" {
		return invalid("unsupported MCP mutation view")
	}
	// Asking for user authority never grants it. Only the host's independently
	// authenticated context may raise the fail-closed model authority.
	if args.stringOr("authority", "") == "user" && options.commandContext != nil && options.commandContext.Authenticated && options.commandContext.UserAuthority && options.commandContext.Principal != "" {
		request.Authority = AuthorityUser
	}
	scoped := false
	switch verb {
	case "delete", "update", "touch", "reject", "restore":
		var ok bool
		request.ID, ok = args.decimalID("id")
		if !ok {
			return invalid("memory." + verb + " requires a positive integer id")
		}
		request.Operation = map[string]string{"delete": "delete-as", "update": "update-as", "touch": "touch", "reject": "reject", "restore": "restore"}[verb]
		if verb == "update" {
			var refusal map[string]any
			request.ExpectedVersion, request.IdempotencyKey, refusal = commandCorrectionOptions(args, request.ID, options.commandContext)
			if refusal != nil {
				return commandResult(refusal)
			}
			options.publicWrite = true
			request.Content = args.stringOr("content", "")
			if request.Content == "" {
				return invalid("missing content")
			}
		}
		if verb == "reject" {
			request.Reason = args.stringOr("reason", "")
		}
		if verb == "restore" {
			if options.commandContext == nil || !options.commandContext.Authenticated {
				return commandResult(commandError("unauthorized", "authenticated user required"))
			}
			request.Actor = options.commandContext.Principal
		}
		scoped = commandScope(args, &request)
	case "upsert_workflow":
		request.Operation, request.Workspace, request.SignalType, request.Rule, request.SessionID = "upsert-workflow", args.stringOr("workspace", ""), args.stringOr("signal_type", ""), args.stringOr("rule", ""), args.stringOr("session_id", "")
		if request.Workspace == "" || request.SignalType == "" || request.Rule == "" {
			return invalid("missing workspace, signal_type or rule")
		}
		confidence := 0.6
		if n, ok := args.number("observed_confidence"); ok {
			confidence = n
		}
		if confidence < 0 || confidence > 1 {
			return invalid("observed_confidence must be between zero and one")
		}
		request.Confidence = &confidence
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory module unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if refusal := commandMutationRefusal(response.Code); refusal != nil {
		return commandResult(refusal)
	}
	result := map[string]any{"status": "ok"}
	if response.MutationReceipt != nil {
		result["mutation_receipt"] = response.MutationReceipt
	}
	missing := false
	switch verb {
	case "delete":
		missing = !response.Deleted
		if args.stringOr("view", "") == "server" {
			result["id"], result["store"], result["deleted"], result["destroyed"] = request.ID, "kb", true, request.Authority == AuthorityUser
		}
	case "touch":
		missing = response.Count == nil || *response.Count == 0
	case "reject", "restore":
		missing = !response.Updated
	case "update":
		if response.Code == nil {
			return nil, bus.ModuleStatusInternal
		}
		switch *response.Code {
		case MutationImmutableExperience:
			return commandResult(commandError("conflict", "episode and experience memories are immutable"))
		case MutationRequiresReplacement:
			return commandResult(commandError("conflict", "instruction and policy memories require a replacement"))
		case MutationOK:
			if len(response.IDs) != 1 || response.IDs[0] <= 0 {
				return nil, bus.ModuleStatusInternal
			}
			result["id"], result["superseded"] = response.IDs[0], response.IDs[0] != request.ID
		default:
			missing = true
		}
	case "upsert_workflow":
		if len(response.Records) != 1 || response.Records[0].ID <= 0 {
			return nil, bus.ModuleStatusInternal
		}
		result["id"] = response.Records[0].ID
	}
	if missing {
		return commandResult(commandError("not_found", "memory not found or mutation refused"))
	}
	if verb == "restore" {
		result["id"], result["restored"] = request.ID, true
	}
	if verb == "reject" && args.stringOr("view", "") == "server" {
		result["id"], result["tombstoned"] = request.ID, true
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	if args.stringOr("view", "") == "mcp" {
		newID := request.ID
		if verb == "update" {
			newID = response.IDs[0]
		}
		return mutationMCPResult(verb, request.ID, newID, "", response.MutationReceipt)
	}
	return commandResult(result)
}

func mutationMCPResult(verb string, id, newID int64, key string, receipts ...*MemoryMutationReceipt) ([]byte, bus.ModuleStatus) {
	var text string
	switch verb {
	case "store":
		text = fmt.Sprintf("stored memory id=%d key=%s", id, key)
	case "update":
		text = fmt.Sprintf("updated memory id=%d", id)
		if newID != id {
			text += fmt.Sprintf(" (previous content kept as a version; current value is now id=%d)", newID)
		}
	case "supersede":
		text = fmt.Sprintf("superseded id=%d new id=%d", id, newID)
	case "delete":
		text = fmt.Sprintf("forgot memory id=%d (retired, not destroyed: it no longer answers recall but remains in fact history)", id)
	case "touch":
		text = fmt.Sprintf("affirmed memory id=%d", id)
	case "reject":
		text = fmt.Sprintf("rejected memory id=%d", id)
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	result := map[string]any{"status": "ok", "text": text, "audit_id": fmt.Sprint(newID)}
	if len(receipts) > 0 && receipts[0] != nil {
		result["mutation_receipt"] = receipts[0]
		if receipts[0].Replayed {
			delete(result, "audit_id")
		}
	}
	return commandResult(result)
}

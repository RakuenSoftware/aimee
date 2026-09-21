package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// The memory, its extraction authority, and its extraction job are one commit.
// A capture or enqueue failure must never acknowledge a partially stored note.
func (s *postgresDataStore) captureStoredFactActor(ctx context.Context, id int64, authority int, caller *bus.CommandContext) error {
	actor := FactActor{Principal: "system:model-inference", Role: "model", Rank: 10, TransportIdentity: "internal"}
	if authority == AuthorityUser {
		if caller == nil || !caller.Authenticated || !caller.UserAuthority {
			return errors.New("memory: verified user authority required")
		}
		actor = FactActor{Principal: caller.Principal, Role: "user", Rank: 30, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
		if actor.TransportIdentity == "" {
			actor.TransportIdentity = actor.Principal
		}
	}
	_, err := s.db.Exec(ctx, `INSERT INTO memory_fact_actors(memory_id,actor_principal,actor_role,authority_rank,authenticated,transport_identity)
VALUES($1,$2,$3,$4,$5,$6) ON CONFLICT(memory_id) DO NOTHING`, id, actor.Principal, actor.Role, actor.Rank, actor.Authenticated, actor.TransportIdentity)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(ctx, `INSERT INTO kb_async_jobs(kind,document_id,project,status,updated_at)
VALUES('memory_facts',$1,'memory','pending',pg_now_text()) ON CONFLICT(kind,document_id) DO UPDATE SET
 status='pending',generation=kb_async_jobs.generation+1,attempts=0,claimed_by='',claimed_at='',last_error='',next_attempt_at='',updated_at=pg_now_text()`, id)
	return err
}

func handleStoreCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	request := DataRequest{Operation: "insert-epistemic", PublicView: true, Key: args.stringOr("key", ""), Content: args.stringOr("content", ""),
		Tier: args.stringOr("tier", "L0"), Kind: args.stringOr("kind", "fact"), EpistemicKind: args.stringOr("epistemic_kind", "world_fact"),
		SessionID: args.stringOr("session_id", ""), UseCases: args.stringOr("use_cases", "")}
	if args.stringOr("view", "") == "mcp" {
		request.Tier = args.stringOr("tier", "L2")
	}
	if strings.TrimSpace(request.Key) == "" || strings.TrimSpace(request.Content) == "" {
		return invalid("memory.store requires non-empty key and content")
	}
	if !validEpistemicKinds[request.EpistemicKind] {
		return invalid("invalid epistemic_kind")
	}
	confidence := 1.0
	if _, exists := args["confidence"]; exists {
		var ok bool
		confidence, ok = args.number("confidence")
		if !ok || confidence < 0 || confidence > 1 {
			return invalid("memory.store confidence must be between 0 and 1")
		}
	}
	request.Confidence = &confidence
	if args.stringOr("authority", "") == "user" && options.commandContext != nil && options.commandContext.UserAuthority {
		request.Authority = AuthorityUser
	}
	scoped := commandScope(args, &request)
	writeContext := strings.TrimSpace(request.Project)
	if writeContext == "" {
		writeContext = strings.TrimSpace(request.Workspace)
	}
	if writeContext == missingScopeValue {
		result := commandError("invalid_argument", "memory.store requires an active project/workspace or explicit scope=all")
		result["reason"], result["active_context_missing"] = "active_context_missing", true
		return commandResult(result)
	}
	options.publicWrite = true
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "failed to store memory"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if refusal := commandMutationRefusal(response.Code, response.Proposal); refusal != nil {
		return commandResult(refusal)
	}
	if len(response.PublicRecords) != 1 {
		return nil, bus.ModuleStatusInternal
	}
	r := response.PublicRecords[0]
	if args.stringOr("view", "") == "mcp" {
		return mutationMCPResult("store", r.ID, r.ID, r.Key)
	}
	result := map[string]any{"status": "ok", "id": r.ID, "memory": r}
	if args.stringOr("view", "") == "native" {
		result["id_text"] = fmt.Sprint(r.ID)
	}
	if args.stringOr("view", "") == "server" {
		result["store"] = "kb"
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

func handleSupersedeCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	id, ok := args.decimalID("old_id")
	content := args.stringOr("new_content", "")
	if !ok || strings.TrimSpace(content) == "" {
		return commandResult(commandError("invalid_argument", "memory.supersede requires old_id and new_content"))
	}
	confidence := 1.0
	if _, exists := args["confidence"]; exists {
		confidence, ok = args.number("confidence")
		if !ok || confidence < 0 || confidence > 1 {
			return commandResult(commandError("invalid_argument", "confidence must be between 0 and 1"))
		}
	}
	expected, key, refusal := commandCorrectionOptions(args, id, options.commandContext)
	if refusal != nil {
		return commandResult(refusal)
	}
	request := DataRequest{IdempotencyKey: key, ExpectedVersion: expected, Operation: "supersede", ID: id, Content: content, Confidence: &confidence, SessionID: args.stringOr("session_id", ""), PublicView: true, IncludeAll: true}
	if caller := options.commandContext; args.stringOr("authority", "") == "user" && caller != nil && caller.Authenticated && caller.UserAuthority && caller.Principal != "" {
		request.Authority = AuthorityUser
	}
	scoped := commandScope(args, &request)
	options.publicWrite = true
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "failed to supersede memory"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if refusal := commandMutationRefusal(response.Code, response.Proposal); refusal != nil {
		return commandResult(refusal)
	}
	if response.Code != nil {
		if *response.Code == MutationImmutableExperience {
			return commandResult(commandError("conflict", errImmutableExperience.Error()))
		}
		if *response.Code == MutationRequiresReplacement {
			return commandResult(commandError("conflict", errRequiresRevocation.Error()))
		}
		return nil, bus.ModuleStatusInternal
	}
	if len(response.PublicRecords) == 0 {
		return commandResult(commandError("not_found", "memory not found or replacement refused"))
	}
	if len(response.PublicRecords) != 1 {
		return nil, bus.ModuleStatusInternal
	}
	if args.stringOr("view", "") == "server" {
		return commandResult(struct {
			Status          string                 `json:"status"`
			Store           string                 `json:"store"`
			MutationReceipt *MemoryMutationReceipt `json:"mutation_receipt,omitempty"`
			publicMemoryRecord
		}{"ok", "kb", response.MutationReceipt, response.PublicRecords[0]})
	}
	result := map[string]any{"status": "ok", "memory": response.PublicRecords[0]}
	if response.MutationReceipt != nil {
		result["mutation_receipt"] = response.MutationReceipt
	}
	if args.stringOr("view", "") == "mcp" {
		return mutationMCPResult("supersede", id, response.PublicRecords[0].ID, "", response.MutationReceipt)
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

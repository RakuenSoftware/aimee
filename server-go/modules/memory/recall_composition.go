package memory

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
)

// Composition is local to the Server placement. The host transports an already
// scoped KB envelope; personal rows never travel back to the shared owner.
func handleRecallComposition(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || options.placement != PlacementServer {
		return nil, bus.ModuleStatusInvalidRequest
	}
	shared, ok := args.stringValue("shared_json")
	if !ok || len(shared) == 0 || len(shared) > maxDataBody/2 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{Operation: "compose-recall", SharedRecall: json.RawMessage(shared)}
	request.SessionStart = args.boolean("session_start")
	request.LimitTokens = recallTokenLimit(args.integer("limit_tokens", 0), request.SessionStart)
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	raw, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInvalidRequest || status == bus.ModuleStatusCancelled {
			return nil, status
		}
		refusal, _ := json.Marshal(commandError("unavailable", "memory recall composition unavailable"))
		return commandResult(map[string]any{"status": "ok", "json": string(refusal)})
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	// A string carries exact int64 tokens through native JSON transport.
	return nativeRecallText(response.Payload, args)
}

func (s *postgresDataStore) ComposeRecall(ctx context.Context, shared json.RawMessage, tokens int, sessionStart bool) (json.RawMessage, error) {
	if s.placement != PlacementServer {
		return nil, errors.New("memory: recall composition requires personal placement")
	}
	var envelope map[string]json.RawMessage
	if len(shared) == 0 || len(shared) > maxDataBody/2 || json.Unmarshal(shared, &envelope) != nil || envelope == nil {
		return nil, errors.New("memory: invalid shared recall envelope")
	}
	var status string
	if json.Unmarshal(envelope["status"], &status) != nil {
		return nil, errors.New("memory: shared recall missing status")
	}
	if status != "ok" {
		if status == "error" || status == "degraded" || status == "quarantined" {
			return shared, nil
		}
		return nil, errors.New("memory: invalid shared recall status")
	}
	var bundle recallBundle
	if json.Unmarshal(envelope["recall"], &bundle) != nil || bundle.Identity == nil || bundle.Preferences == nil || bundle.ActiveContext == nil || bundle.OpenCommitments == nil || bundle.AlwaysOnRules == nil || bundle.Reminders == nil || bundle.Directives == nil {
		return nil, errors.New("memory: incomplete shared recall bundle")
	}
	personalCollection, err := s.observeRecallCollection(ctx)
	if err != nil {
		return nil, err
	}
	bundle.PersonalCollection = personalCollection
	identity, err := s.recallRecords(ctx, `kind='fact' AND tier IN ('L2','L3','L4','L5') AND
 (key LIKE 'identity:%' OR key LIKE 'name:%' OR key LIKE 'role:%' OR key LIKE 'user:%' OR key LIKE 'self:%')`, 32)
	if err != nil {
		return nil, err
	}
	preferences, err := s.recallRecords(ctx, `kind='preference' AND tier IN ('L2','L3','L4','L5')`, 32)
	if err != nil {
		return nil, err
	}
	bundle.Identity = composeRecallSection(identity, bundle.Identity, "user identity")
	bundle.Preferences = composeRecallSection(preferences, bundle.Preferences, "user preference")
	bundle.LimitTokens, bundle.SessionStart = recallTokenLimit(tokens, sessionStart), sessionStart
	bundle.BudgetExceeded = false
	body, err := bundle.encodeBudgeted()
	if err != nil {
		return nil, err
	}
	envelope["recall"] = body
	envelope["store"] = json.RawMessage(`"composed"`)
	return json.Marshal(envelope)
}

// Personal preferences override shared rows with the same key within a section.
// IDs are placement-scoped: equal numbers alone never deduplicate two memories.
func composeRecallSection(personal []Record, shared []RecallRecord, why string) []RecallRecord {
	result := recallItems(personal)
	keys := make(map[string]bool, len(personal))
	for i := range result {
		result[i].Why = why
		keys[result[i].Key] = true
	}
	for _, r := range shared {
		if !keys[r.Key] {
			result = append(result, r)
		}
	}
	return result
}

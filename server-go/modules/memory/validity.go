package memory

import (
	"context"
	"encoding/json"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// EligibilityDecision describes the actual read predicate at one database
// statement snapshot. Unknown evidence/authority is not inferred from relevance.
// It is an observation, not authorization for a subsequent provider dispatch.
type EligibilityDecision struct {
	SchemaVersion         int                  `json:"schema_version"`
	PolicyVersion         string               `json:"policy_version"`
	Mode                  string               `json:"mode"`
	Eligible              bool                 `json:"eligible"`
	ReasonCodes           []string             `json:"reason_codes"`
	Lifecycle             string               `json:"lifecycle"`
	TemporalApplicability string               `json:"temporal_applicability"`
	EvidenceState         string               `json:"evidence_state"`
	AuthorityClass        string               `json:"authority_class"`
	CheckedAt             string               `json:"checked_at,omitempty"`
	ValidAt               string               `json:"valid_at,omitempty"`
	Version               *MemoryRecordVersion `json:"checked_version,omitempty"`
}

func personalCurrentMemorySQL(prefix string) string {
	return prefix + `lifecycle_state='active' AND (` + prefix + `valid_until IS NULL OR ` + prefix + `valid_until>CURRENT_TIMESTAMP)`
}

func (s *postgresDataStore) validity(ctx context.Context, id int64, policy *MemoryReadResult) (EligibilityDecision, error) {
	result := EligibilityDecision{SchemaVersion: 1, PolicyVersion: currentEligibilityPolicy, Mode: policy.Mode,
		ReasonCodes: []string{"not_found_or_unauthorized"}, Lifecycle: "unknown", TemporalApplicability: "unknown", EvidenceState: "unknown", AuthorityClass: "unknown", ValidAt: policy.ValidAt}
	version := &MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}
	var lifecycle, clock string
	var eligible, started, unexpired, suppressed, inputs bool
	var err error
	if s.placement == PlacementServer {
		err = s.db.QueryRow(ctx, `SELECT lifecycle_state,(`+personalCurrentMemorySQL("")+`),true,
   (valid_until IS NULL OR valid_until>CURRENT_TIMESTAMP),false,true,
   (SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1),record_revision::text,CURRENT_TIMESTAMP::text
   FROM user_memories WHERE id=$1`, id).Scan(&lifecycle, &eligible, &started, &unexpired, &suppressed, &inputs, &version.OwnerID, &version.RecordRevision, &clock)
	} else {
		predicate := currentMemorySQL("m.")
		at := "CURRENT_TIMESTAMP"
		params := []any{id}
		historical := policy.Mode == "historical"
		if historical {
			predicate = historicalMemoryInspectionSQL("m.")
			at = "$2::timestamptz"
			params = append(params, policy.ValidAt)
			predicate += " AND " + memoryValidityAtSQL("m.", at)
		}
		err = s.db.QueryRow(ctx, `SELECT m.lifecycle_state,(`+predicate+`),`+memoryStartedAtSQL("m.valid_from", at)+`,`+memoryUnexpiredAtSQL("m.valid_until", at)+`,
   m.activation_suppressed<>0,(`+currentEpisodeCardInputsSQL("m.", historical)+`),
   (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),m.record_revision::text,CURRENT_TIMESTAMP::text
   FROM memories m WHERE m.id=$1`, params...).Scan(&lifecycle, &eligible, &started, &unexpired, &suppressed, &inputs, &version.OwnerID, &version.RecordRevision, &clock)
	}
	if store.IsNoRows(err) {
		return result, nil
	}
	if err != nil {
		return result, err
	}
	result.Eligible, result.Lifecycle, result.CheckedAt, result.Version = eligible, lifecycle, clock, version
	result.TemporalApplicability = "applicable"
	result.ReasonCodes = []string{}
	if !started {
		result.TemporalApplicability = "future"
		result.ReasonCodes = append(result.ReasonCodes, "not_yet_valid")
	}
	if !unexpired {
		result.TemporalApplicability = "expired"
		result.ReasonCodes = append(result.ReasonCodes, "expired")
	}
	retained := policy.Mode == "historical" && (lifecycle == "superseded" || lifecycle == "archived" || lifecycle == "retired")
	if lifecycle != "active" && !retained {
		result.ReasonCodes = append(result.ReasonCodes, "lifecycle_excluded")
	}
	if suppressed && !retained {
		result.ReasonCodes = append(result.ReasonCodes, "suppressed")
	}
	if !inputs {
		result.ReasonCodes = append(result.ReasonCodes, "derived_inputs_unavailable")
	}
	if eligible {
		result.ReasonCodes = []string{"eligible"}
	}
	return result, nil
}

func handleValidityCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	var ok bool
	args, ok = commandDomainArgs(args, "memory.validity")
	if !ok {
		return invalid("invalid validity envelope")
	}
	for key := range args {
		switch key {
		case "id", "mode", "valid_at", "believed_at", "scope", "project", "workspace":
		default:
			return invalid("unrecognized validity argument")
		}
	}
	id, ok := args.decimalID("id")
	if !ok {
		return invalid("validity requires a positive integer id")
	}
	policy := &MemoryReadPolicy{SchemaVersion: 1, Mode: args.stringOr("mode", "current"), ValidAt: args.stringOr("valid_at", ""), BelievedAt: args.stringOr("believed_at", "")}
	request := DataRequest{Operation: "validity", ID: id, ReadPolicy: policy}
	for _, field := range []string{"mode", "valid_at", "believed_at", "project", "workspace"} {
		if raw, exists := args[field]; exists {
			var value *string
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return invalid(field + " must be a string")
			}
		}
	}
	request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
	if raw, exists := args["scope"]; exists {
		var scope *Scope
		if json.Unmarshal(raw, &scope) != nil || scope == nil || scope.Type == "" {
			return invalid("scope requires a type")
		}
		if request.Project != "" || request.Workspace != "" {
			return invalid("scope cannot be combined with project or workspace")
		}
		request.Scope = *scope
	}
	if options.placement == PlacementKB && request.Scope.Type == "" && request.Project == "" && request.Workspace == "" {
		if caller := options.commandContext; caller != nil && caller.Authenticated && caller.ScopeKind != "" {
			request.Scope = Scope{Type: caller.ScopeKind, Value: caller.ScopeID}
		}
	}
	if options.placement == PlacementServer {
		if _, exists := args["scope"]; exists || request.Project != "" || request.Workspace != "" {
			return invalid("personal validity does not accept shared scope")
		}
		request.Scope = Scope{Type: ScopeUser}
	}
	raw, _ := json.Marshal(request)
	body, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "validity owner unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(body, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if response.Read != nil && response.Read.ErrorCode != "" {
		return commandResult(commandError(response.Read.ErrorCode, response.Read.Message))
	}
	if len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

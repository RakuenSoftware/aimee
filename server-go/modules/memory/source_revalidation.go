package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const maxReleaseSources = 512

type sourceRevalidation struct {
	SchemaVersion int                  `json:"schema_version"`
	CheckID       string               `json:"check_id"`
	SendGuard     string               `json:"send_guard,omitempty"`
	Sources       []typedProjectionRef `json:"sources"`
}

func (r *sourceRevalidation) valid() bool {
	if r == nil || (r.SendGuard != "" && r.SendGuard != "acquire" && r.SendGuard != "release") || r.SchemaVersion != 1 || !releaseTokenValid(r.CheckID) || len(r.Sources) > maxReleaseSources {
		return false
	}
	if len(r.Sources) == 0 {
		return r.SendGuard == "release"
	}
	for _, ref := range r.Sources {
		if ref.Source == nil || ref.Source.MemoryParentState != "observed" || !validTypedSource(ref) ||
			(ref.Channel == "historical_assertions" && ref.Source.ReadPolicy == nil) {
			return false
		}
	}
	return true
}

// All roots, the complete direct-parent sets, owner identity and current read
// eligibility are compared in ONE statement snapshot. Missing and RLS-hidden
// records both refuse, without revealing which record failed. This is a read
// check, not a lock spanning a remote provider call or a durable dispatch receipt.
func (s *postgresDataStore) revalidateSources(ctx context.Context, request *sourceRevalidation, exact Scope) (bool, error) {
	if s.placement != PlacementKB || !request.valid() {
		return false, errors.New("memory: invalid source revalidation")
	}
	refs, err := json.Marshal(request.Sources)
	if err != nil {
		return false, err
	}
	var count int
	query := sourceRevalidationSQL
	structured, rules, auxiliary := false, false, false
	for _, ref := range request.Sources {
		switch ref.Source.Kind {
		case "learning_observation", "learning_procedure", "memory_relation":
			auxiliary = true
		case "memory_rule", "memory_rule_collection":
			rules = true
		case "memory_directive", "memory_reminder":
			structured = true
		}
	}
	if auxiliary {
		query = auxiliarySourceRevalidationSQL
	} else if rules {
		query = ruleSourceRevalidationSQL
	} else if structured {
		query = structuredSourceRevalidationSQL
	}
	if request.SendGuard == "acquire" {
		query = strings.ReplaceAll(query, "CURRENT_TIMESTAMP", "clock_timestamp()")
	}
	err = s.db.QueryRow(ctx, query, string(refs), exact.Type, exact.Value).Scan(&count)
	if err == nil && count == len(request.Sources) && request.SendGuard == "acquire" {
		query = strings.ReplaceAll(query, "clock_timestamp()", "(clock_timestamp()+interval '5 seconds')")
		err = s.db.QueryRow(ctx, query, string(refs), exact.Type, exact.Value).Scan(&count)
	}
	return err == nil && count == len(request.Sources), err
}

// Compile fixed owner SQL once. Facts inspect all evidence locators; typed
// assertions inspect live evidence. Both share the same bounded parent probes.
var sourceRevalidationSQL = buildSourceRevalidationSQL(false)
var structuredSourceRevalidationSQL = buildSourceRevalidationSQL(true)

var ruleSourceRevalidationSQL = strings.Replace(structuredSourceRevalidationSQL,
	" WHEN 'memory_directive'", ` WHEN 'memory_rule_collection' THEN EXISTS (
 SELECT 1 FROM memory_collection_owner WHERE id=1
 AND rules_revision::text=r.ref#>>'{source_version,version,record_revision}')
 WHEN 'memory_rule' THEN EXISTS (SELECT 1 FROM rules WHERE id=(r.ref->>'stable_id')::bigint
 AND directive_type='hard' AND `+memoryUnexpiredAtSQL("expires_at", "CURRENT_TIMESTAMP")+`
 AND record_revision::text=r.ref#>>'{source_version,version,record_revision}')
 WHEN 'memory_directive'`, 1)

// Auxiliary learning tables have explicit scope fields instead of memory-row
// RLS. Read them against the authenticated transaction audience and exact scope.
var auxiliarySourceScopeSQL = strings.NewReplacer(
	"$1", "(current_setting('aimee.memory_scope_all',true)='1')",
	"$2", "COALESCE(current_setting('aimee.memory_workspace',true),'')",
	"$3", "COALESCE(current_setting('aimee.memory_project',true),'')",
	"$4", "$2::text", "$5", "$3::text",
).Replace(typedScopeSQL)

var auxiliarySourceRevalidationSQL = strings.Replace(ruleSourceRevalidationSQL,
	" WHEN 'memory_rule_collection'", ` WHEN 'learning_observation' THEN EXISTS (
 SELECT 1 FROM learning_observations WHERE observation_id=r.ref->>'stable_id'
 AND memory_record_id::text=r.ref#>>'{source_version,version,record_id}'
 AND record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND status='active' AND `+auxiliarySourceScopeSQL+`)
 WHEN 'learning_procedure' THEN EXISTS (
 SELECT 1 FROM (SELECT id,record_revision,state,sink,action_json,
 (CASE WHEN action_json IS JSON OBJECT THEN action_json::jsonb ELSE '{}'::jsonb END)->>'scope_kind' AS scope_kind,
 (CASE WHEN action_json IS JSON OBJECT THEN action_json::jsonb ELSE '{}'::jsonb END)->>'scope_id' AS scope_id
 FROM learning_proposals) p WHERE id=(r.ref->>'stable_id')::bigint
 AND record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND state='committed' AND sink='artifact' AND action_json IS JSON OBJECT AND `+auxiliarySourceScopeSQL+`)
 WHEN 'memory_relation' THEN EXISTS (
 SELECT 1 FROM memory_relations relation WHERE id=(r.ref#>>'{source_version,version,record_id}')::bigint
 AND record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND (lower(src_entity)=lower(r.ref->>'stable_id') OR lower(dst_entity)=lower(r.ref->>'stable_id'))
 AND `+currentRelationInputsSQL("relation")+` AND `+relationValidityAtSQL("relation.", "CURRENT_TIMESTAMP")+`
 AND EXISTS(SELECT 1 FROM memories m WHERE m.id=relation.memory_id AND `+currentMemorySQL("m.")+` AND ($2::text='' OR (m.scope_type=$2 AND m.scope_value=$3)))
 AND (`+relationSourceParentsSQL("relation")+`)::jsonb=COALESCE(r.ref#>'{source_version,memory_parents}','[]'::jsonb))
 WHEN 'memory_rule_collection'`, 1)

func buildSourceRevalidationSQL(structured bool) string {
	filter := strings.NewReplacer(
		"$1", "COALESCE(r.ref#>>'{source_version,read_policy,believed_at}','')",
		"$2", "COALESCE(r.ref#>>'{source_version,read_policy,valid_at}','')",
		"$3", "COALESCE((r.ref#>>'{source_version,read_policy,include_historical}')::boolean,false)",
		"$5", "$2::text", "$6", "$3::text",
	).Replace(strings.ReplaceAll(assertionFilter, " AND f.invalidated_at=''", " AND (r.ref->>'channel'='facts' OR f.invalidated_at='')"))
	expectedParents := `COALESCE((SELECT jsonb_agg(jsonb_build_object('record_id',p->>'record_id',
 'record_revision',p->>'record_revision') ORDER BY (p->>'record_id')::bigint)
 FROM jsonb_array_elements(COALESCE(r.ref#>'{source_version,memory_parents}','[]'::jsonb)) p),'[]'::jsonb)`
	structuredCases := ""
	if structured {
		structuredCases = ` WHEN 'memory_directive' THEN EXISTS (
 SELECT 1 FROM epistemic_directives WHERE id=(r.ref->>'stable_id')::bigint
 AND state='open' AND ` + memoryUnexpiredSQL("") + ` AND ` + currentDirectiveParentsSQL("epistemic_directives") + `
 AND epistemic_directives.record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND (` + directiveSourceParentsSQL("epistemic_directives") + `)::jsonb=COALESCE(r.ref#>'{source_version,memory_parents}','[]'::jsonb))
 WHEN 'memory_reminder' THEN EXISTS (
 SELECT 1 FROM prospective_memories WHERE id=(r.ref->>'stable_id')::bigint
 AND (state='armed' OR (state='triggered' AND recurrence='once'
 AND COALESCE((r.ref#>>'{source_version,read_policy,retained_reminder}')::boolean,false))) AND ` + memoryUnexpiredSQL("") + `
 AND prospective_memories.record_revision::text=r.ref#>>'{source_version,version,record_revision}')
`
	}

	return `SELECT count(*) FROM jsonb_array_elements($1::jsonb) r(ref)
 WHERE r.ref#>>'{source_version,version,owner_id}'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND (CASE r.ref#>>'{source_version,record_kind}'
` + structuredCases + ` WHEN 'semantic_assertion' THEN EXISTS (
 SELECT 1 FROM entity_edges e WHERE e.id=(r.ref->>'stable_id')::bigint
 AND e.version::text=r.ref#>>'{source_version,version,record_revision}'
 AND ` + filter + ` AND (` + assertionMemoryVersions + `)::jsonb=` + expectedParents + `)
 ELSE EXISTS (
 SELECT 1 FROM memories m WHERE
 m.id=(CASE WHEN r.ref#>>'{source_version,record_kind}'='memory_record'
 THEN r.ref->>'stable_id' ELSE r.ref#>>'{source_version,memory_parents,0,record_id}' END)::bigint
 AND m.record_revision::text=CASE WHEN r.ref#>>'{source_version,record_kind}'='memory_record'
 THEN r.ref#>>'{source_version,version,record_revision}' ELSE r.ref#>>'{source_version,memory_parents,0,record_revision}' END
 AND (CASE WHEN r.ref->>'channel'='native_open_commitments'
 THEN m.lifecycle_state='pending' AND m.activation_suppressed=0 AND ` + memoryValiditySQL("m.") + `
 ELSE ` + currentMemorySQL("m.") + ` END) AND ($2::text='' OR (m.scope_type=$2 AND m.scope_value=$3))
 AND CASE r.ref#>>'{source_version,record_kind}'
 WHEN 'memory_record' THEN true
 WHEN 'memory_episode' THEN EXISTS (SELECT 1 FROM memory_episodes e WHERE e.id=(r.ref->>'stable_id')::bigint
 AND e.memory_id=m.id AND e.record_revision::text=r.ref#>>'{source_version,version,record_revision}' AND ` + currentEpisodeInputsSQL("e") + `)
 WHEN 'memory_summary' THEN EXISTS (SELECT 1 FROM memory_summaries s WHERE s.id=(r.ref->>'stable_id')::bigint
 AND s.memory_id=m.id AND s.record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND ` + summaryCurrentInputsSQL("s", "m") + `)
 ELSE false END)
 END)`
}

func handleSourceRevalidation(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	request := DataRequest{Operation: "source-revalidate"}
	if json.Unmarshal(args["revalidation"], &request.Revalidation) != nil || !request.Revalidation.valid() {
		return commandResult(commandError("invalid_argument", "invalid source revalidation"))
	}
	if request.Revalidation.SendGuard != "" && !sourceSendGuardAllowed(options.commandContext) {
		return commandResult(commandError("unauthorized", "send guards require a verified service or owner"))
	}

	if !commandScope(args, &request) || request.IncludeAll {
		return commandResult(commandError("invalid_argument", "source revalidation requires scoped context"))
	}
	raw, _ := json.Marshal(request)
	result, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "source owner unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(result, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

// The caller owns a storage transaction, including rollback on refusal. The
// lease and its successful source observation must commit together.
func (s *postgresDataStore) guardedSourceRevalidation(ctx context.Context, request *sourceRevalidation, exact Scope) (bool, error) {
	if request.SendGuard == "release" {
		_, err := s.db.Exec(ctx, "SELECT memory_send_guard_end($1)", request.CheckID)
		return err == nil, err
	}
	if request.SendGuard == "acquire" {
		if _, err := s.db.Exec(ctx, "SELECT memory_send_guard_begin($1,5000)", request.CheckID); err != nil {
			return false, err
		}
	}
	if s.placement == PlacementServer {
		return s.revalidatePersonalSources(ctx, request)
	}
	return s.revalidateSources(ctx, request, exact)
}
func sourceGuardResponse(request *sourceRevalidation, eligible bool) map[string]any {
	result := map[string]any{"status": "ok", "eligible": eligible, "check_id": request.CheckID, "sources_digest": releaseDigest(request.Sources)}
	if eligible && request.SendGuard == "acquire" {
		result["send_guard"] = "acquired"
		result["guard_schema_version"] = 2
		result["lease_ms"] = 5000
	}
	if eligible && request.SendGuard == "release" {
		result["send_guard"] = "released"
		result["guard_schema_version"] = 2
	}
	return result
}

// Read-scoped credentials cannot acquire a store-wide mutation barrier. This
// authority comes from the verified host frame, never the request arguments.
func sourceSendGuardAllowed(caller *bus.CommandContext) bool {
	return caller != nil && caller.Authenticated && caller.Principal != "" &&
		((caller.ScopeKind == "service" && caller.ScopeID != "") ||
			(caller.ScopeKind == "" && caller.ScopeID == "" && caller.UserAuthority))
}

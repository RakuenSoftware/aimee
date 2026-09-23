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
	Sources       []typedProjectionRef `json:"sources"`
}

func (r *sourceRevalidation) valid() bool {
	if r == nil || r.SchemaVersion != 1 || !releaseTokenValid(r.CheckID) || len(r.Sources) == 0 || len(r.Sources) > maxReleaseSources {
		return false
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
	err = s.db.QueryRow(ctx, sourceRevalidationSQL, string(refs), exact.Type, exact.Value).Scan(&count)
	return err == nil && count == len(request.Sources), err
}

// Compile fixed owner SQL once. Facts inspect all evidence locators; typed
// assertions inspect live evidence. Both share the same bounded parent probes.
var sourceRevalidationSQL = buildSourceRevalidationSQL()

func buildSourceRevalidationSQL() string {
	filter := strings.NewReplacer(
		"$1", "COALESCE(r.ref#>>'{source_version,read_policy,believed_at}','')",
		"$2", "COALESCE(r.ref#>>'{source_version,read_policy,valid_at}','')",
		"$3", "COALESCE((r.ref#>>'{source_version,read_policy,include_historical}')::boolean,false)",
		"$5", "$2::text", "$6", "$3::text",
	).Replace(strings.ReplaceAll(assertionFilter, " AND f.invalidated_at=''", " AND (r.ref->>'channel'='facts' OR f.invalidated_at='')"))
	expectedParents := `COALESCE((SELECT jsonb_agg(jsonb_build_object('record_id',p->>'record_id',
 'record_revision',p->>'record_revision') ORDER BY (p->>'record_id')::bigint)
 FROM jsonb_array_elements(COALESCE(r.ref#>'{source_version,memory_parents}','[]'::jsonb)) p),'[]'::jsonb)`
	return `SELECT count(*) FROM jsonb_array_elements($1::jsonb) r(ref)
 WHERE r.ref#>>'{source_version,version,owner_id}'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND (CASE r.ref#>>'{source_version,record_kind}'
 WHEN 'semantic_assertion' THEN EXISTS (
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
 AND e.memory_id=m.id AND e.record_revision::text=r.ref#>>'{source_version,version,record_revision}')
 WHEN 'memory_summary' THEN EXISTS (SELECT 1 FROM memory_summaries s WHERE s.id=(r.ref->>'stable_id')::bigint
 AND s.memory_id=m.id AND s.record_revision::text=r.ref#>>'{source_version,version,record_revision}')
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

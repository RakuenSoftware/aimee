package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

// A collection observation is captured before selection, including an empty
// result. It commits to the effective audience and the sum of its monotonic
// heads. No hidden collection contributes to a scoped head. Changing the
// audience cannot reuse a coincidentally equal generation.
const collectionAudienceSQL = `(CASE WHEN current_setting('aimee.memory_scope_all',true)='1'
 THEN '[{"type":"all","value":"*"}]'::jsonb ELSE
 (SELECT jsonb_agg(jsonb_build_object('type',t,'value',v) ORDER BY t COLLATE "C",v COLLATE "C")
 FROM (SELECT DISTINCT t,v FROM (VALUES
 ('global','_global'),('workspace','_shared'),
 (COALESCE(current_setting('aimee.memory_scope_type',true),''),COALESCE(current_setting('aimee.memory_scope_value',true),'')),
 ('workspace',COALESCE(current_setting('aimee.memory_workspace',true),'')),
 ('project',COALESCE(current_setting('aimee.memory_project',true),''))) scopes(t,v)
 WHERE t<>'' AND v<>'') visible) END)`

// Zero is a valid collection head. Version envelopes use positive revisions,
// so revision one represents the empty generation. PostgreSQL raises on overflow.
const collectionRevisionSQL = `(SELECT (1+COALESCE(sum(generation),0))::bigint::text FROM (
 SELECT generation FROM memory_collection_generations WHERE memory_row_scope_visible(scope_type,scope_value)
 UNION ALL SELECT generation FROM memory_projection_generations WHERE memory_row_scope_visible(scope_type,scope_value)
 ) visible_heads)`

func (s *postgresDataStore) observeRecallCollection(ctx context.Context) (*typedSourceVersion, error) {
	var owner, revision, audience, deadline string
	kind := "memory_collection"
	var err error
	if s.placement == PlacementServer {
		kind = "user_memory_collection"
		err = s.db.QueryRow(ctx, `SELECT owner_id::text,(generation+1)::text,'[]',`+privateCollectionDeadlineSQL+` FROM user_memory_collection_generation WHERE id=1`).Scan(&owner, &revision, &audience, &deadline)
	} else {
		err = s.db.QueryRow(ctx, `SELECT owner_id::text,`+collectionRevisionSQL+`,`+collectionAudienceSQL+`::text,`+sharedCollectionDeadlineSQL+` FROM memory_collection_owner WHERE id=1`).Scan(&owner, &revision, &audience, &deadline)
	}
	if err != nil {
		return nil, err
	}
	observation := &typedSourceVersion{Kind: kind, Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: "1", RecordRevision: revision}, MemoryParentState: "observed", CollectionValidUntil: deadline}
	if err = json.Unmarshal([]byte(audience), &observation.CollectionAudience); err != nil {
		return nil, err
	}
	if !validTypedSource(typedProjectionRef{Channel: "native_memory_collection", ID: "1", Source: observation}) {
		return nil, fmt.Errorf("memory: invalid collection observation")
	}
	return observation, nil
}

func validCollectionAudience(scopes []Scope) bool {
	if len(scopes) == 1 && scopes[0].Type == "all" && scopes[0].Value == "*" {
		return true
	}
	if len(scopes) < 2 || len(scopes) > 5 {
		return false
	}
	previous := ""
	global, shared := false, false
	for _, scope := range scopes {
		normalized, err := normalizeScope(PlacementKB, scope)
		if err != nil || normalized != scope {
			return false
		}
		key := string(scope.Type) + "\x00" + scope.Value
		if key <= previous {
			return false
		}
		previous = key
		global = global || (scope.Type == ScopeGlobal && scope.Value == "_global")
		shared = shared || (scope.Type == ScopeWorkspace && scope.Value == "_shared")
	}
	return global && shared
}

// Stored counters do not move when a valid-time boundary passes. Bind the view
// to the earliest future boundary in its visible collection, including records
// which did not match the query. The release lease must fit before that boundary.
var sharedCollectionDeadlineSQL = `(SELECT COALESCE(to_char(min(boundary) AT TIME ZONE 'UTC',
 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),'') FROM memories m CROSS JOIN LATERAL
 (VALUES (` + memoryTimeSQL("m.valid_from") + `),(` + memoryTimeSQL("m.valid_until") + `)) limits(boundary)
 WHERE boundary>CURRENT_TIMESTAMP)`

const privateCollectionDeadlineSQL = `(SELECT COALESCE(to_char(min(valid_until) AT TIME ZONE 'UTC',
 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),'') FROM user_memories WHERE valid_until>CURRENT_TIMESTAMP)`

const collectionDeadlineCheckSQL = `(COALESCE(r.ref#>>'{source_version,collection_valid_until}','')=''
 OR CURRENT_TIMESTAMP<(r.ref#>>'{source_version,collection_valid_until}')::timestamptz)`

func validCollectionDeadline(value string) bool {
	if value == "" {
		return true
	}
	if len(value) > 32 || !strings.HasSuffix(value, "Z") {
		return false
	}
	stamp, err := time.Parse(time.RFC3339Nano, value)
	return err == nil && stamp.Year() > 0
}

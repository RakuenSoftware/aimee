package memory

import (
	"context"
	"errors"
)

// A shared cursor belongs to exactly one primary collection. Multi-scope views
// must retain one cursor per collection, including empty collections; a single
// cursor from one scope cannot certify another scope or an all-scope query.
func (s *postgresDataStore) sharedChanges(ctx context.Context, scope Scope, request MemoryChangesRequest) (MemoryChangePage, error) {
	if s.placement != PlacementKB || !request.valid() {
		return MemoryChangePage{}, errors.New("memory: unsupported change feed request")
	}
	scope, err := normalizeScope(PlacementKB, scope)
	if err != nil {
		return MemoryChangePage{}, err
	}
	collection := scope.Type + ":" + scope.Value
	limit := request.Limit
	if limit == 0 {
		limit = 64
	}
	owner, after := "", int64(0)
	if request.After != nil && request.After.Collection == collection {
		owner, after = request.After.OwnerID, request.After.Generation
	}
	// Empty collections have generation zero under the same owner, without
	// inserting a row on reads. Both explicit visibility and storage RLS apply.
	var raw string
	err = s.db.QueryRow(ctx, `WITH head AS MATERIALIZED (
 SELECT o.owner_id::text AS owner_id,COALESCE(g.generation,0) AS generation
 FROM memory_collection_owner o LEFT JOIN memory_collection_generations g
 ON g.scope_type=$1 AND g.scope_value=$2
 WHERE o.id=1 AND memory_row_scope_visible($1,$2)
), events AS (
 SELECT e.generation,e.memory_id,e.record_revision,e.operation
 FROM memory_invalidation_outbox e,head h
 WHERE e.scope_type=$1 AND e.scope_value=$2 AND $3=h.owner_id AND $4<=h.generation
 AND e.generation>$4 AND e.generation<=h.generation
 ORDER BY e.generation LIMIT $5
)
SELECT jsonb_build_object('head',to_jsonb(h),'events',
 COALESCE((SELECT jsonb_agg(to_jsonb(e) ORDER BY e.generation) FROM events e),'[]'::jsonb))::text
 FROM head h`, scope.Type, scope.Value, owner, after, limit).Scan(&raw)
	if err != nil {
		return MemoryChangePage{}, err
	}
	return finishMemoryChangePage(raw, request, limit, collection)
}

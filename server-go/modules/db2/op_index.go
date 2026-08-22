package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityListActive,
		db2contract.OperationEntityListActive, entityListActive)
	Register(db2contract.StageEntityEdgeCoTargets,
		db2contract.OperationEntityEdgeCoTargets, entityEdgeCoTargets)
	Register(db2contract.StageCodeIndexProjectList,
		db2contract.OperationCodeIndexProjectList, codeIndexProjectList)
}

const entityListActiveQuery = `SELECT LOWER(entity), COUNT(DISTINCT memory_id) AS obs
 FROM memory_entities
 GROUP BY LOWER(entity)
 HAVING COUNT(DISTINCT memory_id) >= $1`

// entityListActive lists entities observed in at least so many distinct
// memories.
//
// Distinct is the point: an entity named ten times in one memory has been
// observed once, so the threshold measures spread rather than repetition. The
// grouping lowercases, so what comes back is the key the group was formed on
// and not any spelling that was written.
func entityListActive(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	minimum, err := db2contract.DecodeEntityListActiveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	rows, err := store.Query(ctx, entityListActiveQuery, int64(minimum))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.EntityListActiveRow, 0,
		db2contract.EntityListActiveMaxRows)
	for rows.Next() {
		// The C caller stops at its ceiling rather than asking the database to,
		// and this does the same: the statement has no LIMIT, so a corpus with
		// more entities than the reply holds is truncated here. Reading past the
		// ceiling and discarding would be the same answer for more work; taking
		// fewer would be a different one.
		if len(found) == db2contract.EntityListActiveMaxRows {
			break
		}
		var (
			entity       string
			observations int64
		)
		if err := rows.Scan(&entity, &observations); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.EntityListActiveRow{
			EntityName:       entity,
			ObservationCount: clampToU32(observations),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeEntityListActiveReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// A code-projection edge is visible only while its generation is, and only for
// a project that is current. Written once because both halves of the union need
// it and an edge visible on one side and not the other would be a graph that
// disagrees with itself.
const entityEdgeVisibleProjection = ` AND (COALESCE(edge_origin, '') <> 'code_projection'
 OR EXISTS (SELECT 1 FROM code_projection_generations cpg
 JOIN projects cpp ON cpp.name=cpg.project
 WHERE cpg.id=entity_edges.projection_generation_id
 AND cpg.state='visible' AND cpp.lifecycle_state='current'))`

const entityEdgeCoTargetsQuery = `SELECT target FROM entity_edges
 WHERE source = $1 AND relation = $2 AND weight > $3 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + ` UNION
 SELECT source FROM entity_edges
 WHERE target = $1 AND relation = $2 AND weight > $3 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + ` LIMIT $4`

// entityEdgeCoTargets names what a node shares a relation with.
//
// The edge is followed in both directions, so a node that is the source of one
// edge and the target of another appears once -- the UNION deduplicates.
// Semantic edges are excluded: they are inferred rather than observed, and
// mixing them in would let a guess look like a recorded fact.
//
// The C statement binds the node, relation and weight twice, once per half of
// the union. pgx numbers placeholders rather than positions, so $1..$3 appear
// in both halves and are bound once. Same query, three fewer arguments to get
// out of step with each other.
func entityEdgeCoTargets(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	node, relation, minWeight, err := db2contract.DecodeEntityEdgeCoTargetsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	rows, err := store.Query(ctx, entityEdgeCoTargetsQuery, node, relation, int64(minWeight),
		db2contract.EntityEdgeCoTargetsMaxRows)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.EntityEdgeCoTargetsRow, 0,
		db2contract.EntityEdgeCoTargetsMaxRows)
	for rows.Next() {
		var target string
		if err := rows.Scan(&target); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		// The C loop skips an empty target rather than returning it.
		if target == "" {
			continue
		}
		found = append(found, db2contract.EntityEdgeCoTargetsRow{EdgeTarget: target})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeEntityEdgeCoTargetsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const codeIndexProjectListQuery = `SELECT name, root, scanned_at FROM projects
 WHERE lifecycle_state = 'current' AND root NOT LIKE '%/.%'
 ORDER BY name`

// codeIndexProjectList names the projects the code index holds.
//
// Current generations only, and no project rooted under a dotted directory --
// which quietly hides a project someone rooted inside .cache or a worktree,
// with nothing in the answer saying one was withheld. That is the C behaviour
// and the reply schema's reason already records it.
func codeIndexProjectList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeCodeIndexProjectListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	rows, err := store.Query(ctx, codeIndexProjectListQuery)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.CodeIndexProjectListRow, 0,
		db2contract.CodeIndexProjectListMaxRows)
	for rows.Next() {
		if len(found) == db2contract.CodeIndexProjectListMaxRows {
			break
		}
		var name, root, scannedAt string
		if err := rows.Scan(&name, &root, &scannedAt); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.CodeIndexProjectListRow{
			ProjectName: name,
			ProjectRoot: root,
			ScannedAt:   scannedAt,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeCodeIndexProjectListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

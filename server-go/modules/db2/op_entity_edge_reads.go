package db2

import (
	"context"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityEdgesForEntity,
		db2contract.OperationEntityEdgesForEntity, entityEdgesForEntity)
	Register(db2contract.StageEntityEdgesByToken,
		db2contract.OperationEntityEdgesByToken, entityEdgesByToken)
	Register(db2contract.StageEntityNeighborsWeighted,
		db2contract.OperationEntityNeighborsWeighted, entityNeighborsWeighted)
	Register(db2contract.StageEntityNeighborsFiltered,
		db2contract.OperationEntityNeighborsFiltered, entityNeighborsFiltered)
}

// Both directions, unioned, because an edge is a fact about a pair and the
// entity asked about can be either end of it.
//
// UNION ALL rather than UNION, unlike the co-targets read: this one selects the
// edge identifier, so the two arms cannot produce the same row and there is
// nothing for a UNION to deduplicate -- only work to do finding that out.
const entityEdgesForEntityQuery = `SELECT id, source, relation, target, weight
 FROM entity_edges
 WHERE source = $1 AND edge_class <> 'semantic'` + entityEdgeVisibleProjection + `
 UNION ALL
 SELECT id, source, relation, target, weight
 FROM entity_edges
 WHERE target = $1 AND edge_class <> 'semantic'` + entityEdgeVisibleProjection + `
 LIMIT $2`

// Case-insensitive on all three columns, because a token comes from a person
// rather than from the index. The edge identifier is selected, so heaviest
// first is the only ordering and it needs no deduplication.
const entityEdgesByTokenQuery = `SELECT id, source, relation, target, weight
 FROM entity_edges
 WHERE (LOWER(source) = LOWER($1)
    OR LOWER(target) = LOWER($1)
    OR LOWER(relation) = LOWER($1))
   AND edge_class <> 'semantic'` + entityEdgeVisibleProjection + `
 ORDER BY weight DESC LIMIT $2`

// readEntityEdges runs one of the two edge searches and maps its rows.
func readEntityEdges(ctx context.Context, store Store, query, match string,
	limit int,
) ([]db2contract.EntityEdgesForEntityRow, error) {
	rows, err := store.Query(ctx, query, match, int64(limit))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	edges := make([]db2contract.EntityEdgesForEntityRow, 0, 16)
	for rows.Next() {
		var id, weight int64
		var source, relation, target string
		if scanErr := rows.Scan(
			&id, &source, &relation, &target, &weight); scanErr != nil {
			return nil, scanErr
		}
		edges = append(edges, db2contract.EntityEdgesForEntityRow{
			EdgeID:       uint64(id),
			EdgeWeight:   clampToU32(weight),
			EdgeSource:   source,
			EdgeRelation: relation,
			EdgeTarget:   target,
		})
	}
	return edges, rows.Err()
}

// entityEdgesForEntity lists the edges an entity takes part in.
func entityEdgesForEntity(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, err := db2contract.DecodeEntityEdgesForEntityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	edges, queryErr := readEntityEdges(ctx, store, entityEdgesForEntityQuery, entity,
		pairLimit(limit, db2contract.EntityEdgesForEntityMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityEdgesForEntityReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// entityEdgesByToken searches the graph for a word.
//
// Source, relation or target: a person looking up "depends_on" wants the
// relation, and one looking up "postgres" wants the endpoints, and nothing in
// the token says which.
func entityEdgesByToken(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	token, limit, err := db2contract.DecodeEntityEdgesByTokenRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	found, queryErr := readEntityEdges(ctx, store, entityEdgesByTokenQuery, token,
		pairLimit(limit, db2contract.EntityEdgesByTokenMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	edges := make([]db2contract.EntityEdgesByTokenRow, len(found))
	for index, edge := range found {
		edges[index] = db2contract.EntityEdgesByTokenRow(edge)
	}
	reply, encodeErr := db2contract.EncodeEntityEdgesByTokenReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The age of the utility stamp is computed by the database, guarded by a
// pattern match so an unparseable value answers nothing rather than failing the
// statement.
//
// The C parses that column in C and compares it against the local clock, while
// the value is written by the database's CURRENT_TIMESTAMP -- so the two agree
// only when the database and the caller share a timezone. Computing the
// difference where the value was written removes the question.
//
// The pattern is the format the writer uses. A row in any other spelling is
// treated as unparseable, which is what the C's sscanf does with one.
const entityNeighborsWeightedQuery = `SELECT node, weight, utility_score, touched_at,
 CASE WHEN touched_at ~ '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$'
      THEN GREATEST(EXTRACT(EPOCH FROM
        (CURRENT_TIMESTAMP - touched_at::timestamp)) / 86400.0, 0)
      ELSE NULL END AS age_days
 FROM (
   SELECT target AS node, weight, utility_score,
     COALESCE(utility_touched_at, '') AS touched_at
   FROM entity_edges
   WHERE source = $1 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
   UNION ALL
   SELECT source AS node, weight, utility_score,
     COALESCE(utility_touched_at, '') AS touched_at
   FROM entity_edges
   WHERE target = $1 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
   LIMIT $2
 ) neighbours`

// The half-life the utility decay uses, in days, and the bounds a decayed score
// is clamped to -- the same bounds a bump is allowed to move it between.
const (
	entityUtilityHalfLifeDays = 90.0
	entityUtilityBound        = 5.0
)

// entityNeighborsWeighted lists what an entity is connected to, with how much
// each connection has proved worth.
//
// The effective utility is the raw score decayed by how long ago it was last
// confirmed, so a connection that mattered once and has not been touched since
// fades rather than standing forever. It is only computed when the caller asks
// for utility scoring; otherwise it is zero, because a caller that is not
// scoring should not be handed a number it did not ask to be charged for.
func entityNeighborsWeighted(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, scoringEnabled, err :=
		db2contract.DecodeEntityNeighborsWeightedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityNeighborsWeightedMaxRows
	rows, queryErr := store.Query(ctx, entityNeighborsWeightedQuery,
		entity, int64(pairLimit(limit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	neighbours := make([]db2contract.EntityNeighborsWeightedRow, 0, 16)
	for rows.Next() && len(neighbours) < ceiling {
		var node, touchedAt string
		var weight int64
		var utility float64
		var ageDays *float64
		if scanErr := rows.Scan(
			&node, &weight, &utility, &touchedAt, &ageDays); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		effective := 0.0
		if scoringEnabled != 0 {
			effective = decayedUtility(utility, touchedAt, ageDays)
		}
		neighbours = append(neighbours, db2contract.EntityNeighborsWeightedRow{
			Node:             node,
			Weight:           clampToU32(weight),
			UtilityScore:     utility,
			EffectiveUtility: effective,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityNeighborsWeightedReply(neighbours)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// decayedUtility fades a utility score by the age of the stamp that confirmed
// it.
//
// Three cases answer without decaying, and each means something different:
//
// A zero score decays to zero whatever its age, so there is nothing to compute.
//
// A score with no stamp keeps its raw value. The C calls this a legacy
// sentinel: rows written before the column existed have a score worth
// something and no way to say when, and fading them to nothing would throw away
// what is known because of what is not.
//
// A stamp in the epoch year is the opposite sentinel -- a backfill that could
// not find a real time wrote it deliberately -- and answers zero.
func decayedUtility(raw float64, touchedAt string, ageDays *float64) float64 {
	if raw == 0 {
		return 0
	}
	if touchedAt == "" || ageDays == nil {
		return raw
	}
	if strings.HasPrefix(touchedAt, "1970-") || touchedAt < "1970-" {
		return 0
	}
	decayed := raw * math.Exp(-math.Ln2*(*ageDays)/entityUtilityHalfLifeDays)
	if decayed > entityUtilityBound {
		return entityUtilityBound
	}
	if decayed < -entityUtilityBound {
		return -entityUtilityBound
	}
	return decayed
}

// The relation filter is IN rather than equality, and the second relation is
// allowed to repeat the first: a caller with one relation to match passes it
// twice, which IN collapses. That is what lets one statement serve both of the
// C's two.
const entityNeighborsFilteredQuery = `SELECT node, weight FROM (
   SELECT target AS node, weight FROM entity_edges
   WHERE source = $1 AND relation IN ($2, $3) AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
   UNION ALL
   SELECT source AS node, weight FROM entity_edges
   WHERE target = $1 AND relation IN ($2, $3) AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
 ) neighbours`

// entityNeighborsFiltered lists what an entity is connected to through one or
// two named relations.
//
// The ordering is optional. Heaviest first costs a sort over the whole union,
// and a caller walking every neighbour anyway does not need it -- so the C
// makes it a flag and this keeps it, appending the clause rather than always
// paying for it.
func entityNeighborsFiltered(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, relationA, relationB, orderByWeight, limit, err :=
		db2contract.DecodeEntityNeighborsFilteredRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	query := entityNeighborsFilteredQuery
	if orderByWeight != 0 {
		query += ` ORDER BY weight DESC`
	}
	query += ` LIMIT $4`

	ceiling := db2contract.EntityNeighborsFilteredMaxRows
	rows, queryErr := store.Query(ctx, query, entity, relationA, relationB,
		int64(pairLimit(limit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	neighbours := make([]db2contract.EntityNeighborsFilteredRow, 0, 16)
	for rows.Next() && len(neighbours) < ceiling {
		var node string
		var weight int64
		if scanErr := rows.Scan(&node, &weight); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		neighbours = append(neighbours, db2contract.EntityNeighborsFilteredRow{
			Node:   node,
			Weight: clampToU32(weight),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityNeighborsFilteredReply(neighbours)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

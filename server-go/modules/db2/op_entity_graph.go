package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityNeighbors,
		db2contract.OperationEntityNeighbors, entityNeighbors)
	Register(db2contract.StageEntityOutboundNeighbors,
		db2contract.OperationEntityOutboundNeighbors, entityOutboundNeighbors)
	Register(db2contract.StageEntityTopTargets,
		db2contract.OperationEntityTopTargets, entityTopTargets)
	Register(db2contract.StageEntityTopPartners,
		db2contract.OperationEntityTopPartners, entityTopPartners)
}

// Every read here excludes edge_class 'semantic'. Those edges are inferred from
// similarity rather than asserted, so a graph walk over them would follow
// resemblance and report it as a relationship.
//
// All four also carry entityEdgeVisibleProjection, which keeps code-projection
// edges out unless their generation is visible and their project current. The
// four differ only in which direction they follow and whether they aggregate,
// so the pieces they share are written once.
const (
	entityEdgeOutboundBody = `SELECT target, weight FROM entity_edges
 WHERE source = $1 AND edge_class <> 'semantic'`
	entityEdgeInboundBody = `SELECT source, weight FROM entity_edges
 WHERE target = $1 AND edge_class <> 'semantic'`
)

// neighbourCeiling applies the C's clamp: a limit outside the reply's own
// ceiling becomes the ceiling, and zero means the default rather than none.
func neighbourCeiling(limit uint32, fallback, maximum int) int {
	ceiling := int(limit)
	if ceiling <= 0 {
		ceiling = fallback
	}
	if ceiling > maximum {
		ceiling = maximum
	}
	return ceiling
}

// readNeighbours collects the node-and-weight shape all four share.
func readNeighbours(ctx context.Context, store Store, statement string, args []any,
	ceiling int,
) ([]db2contract.EntityNeighborsRow, bus.ModuleStatus) {
	rows, err := store.Query(ctx, statement, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.EntityNeighborsRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var node *string
		var weight *int64
		if err := rows.Scan(&node, &weight); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.EntityNeighborsRow{
			Node:   text(node),
			Weight: clampToU32(number(weight)),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

// entityNeighbors lists what an entity is connected to, in either direction.
//
// UNION ALL rather than UNION, so an entity connected to another both ways
// appears twice with its two weights rather than being folded into one. That is
// the C behaviour and it matters: the two edges are separate assertions, and
// merging them here would hide that the relationship is reciprocated.
//
// A limit of zero means fifty, not none. The C defaults it before building the
// statement, and a caller that omits a limit wants a neighbourhood rather than
// an empty answer.
func entityNeighbors(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	entity, limit, err := db2contract.DecodeEntityNeighborsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := neighbourCeiling(limit, 50, db2contract.EntityNeighborsMaxRows)
	statement := entityEdgeOutboundBody + entityEdgeVisibleProjection +
		` UNION ALL ` + entityEdgeInboundBody + entityEdgeVisibleProjection +
		` LIMIT $2`

	found, status := readNeighbours(ctx, store, statement,
		[]any{entity, int64(ceiling)}, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeEntityNeighborsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// entityOutboundNeighbors lists only what an entity points at.
//
// The direction is the whole difference from entityNeighbors, and it is the
// difference between "what does this entity assert" and "what is this entity
// involved in".
func entityOutboundNeighbors(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, err := db2contract.DecodeEntityOutboundNeighborsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := neighbourCeiling(limit, 50, db2contract.EntityOutboundNeighborsMaxRows)
	statement := entityEdgeOutboundBody + entityEdgeVisibleProjection + ` LIMIT $2`

	found, status := readNeighbours(ctx, store, statement,
		[]any{entity, int64(ceiling)}, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.EntityOutboundNeighborsRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.EntityOutboundNeighborsRow(row))
	}
	reply, err := db2contract.EncodeEntityOutboundNeighborsReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Both aggregating reads match the entity case-insensitively, unlike the two
// above. That is the C behaviour and the difference is real: these answer a
// question a person asked about a named thing, where the neighbour reads walk
// a graph from a node whose spelling the caller already holds.
const entityTopTargetsQuery = `SELECT target, SUM(weight) AS w FROM entity_edges
 WHERE LOWER(source) = LOWER($1) AND relation = $2 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + ` GROUP BY target ORDER BY w DESC LIMIT $3`

// entityTopTargets ranks what an entity points at under one relation.
//
// Weights are summed per target, so several edges asserting the same thing add
// up rather than competing. Ordered by that sum, which is what makes this a
// ranking rather than a listing.
func entityTopTargets(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	entity, relation, err := db2contract.DecodeEntityTopTargetsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityTopTargetsMaxRows
	found, status := readNeighbours(ctx, store, entityTopTargetsQuery,
		[]any{entity, relation, int64(ceiling)}, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.EntityTopTargetsRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.EntityTopTargetsRow(row))
	}
	reply, err := db2contract.EncodeEntityTopTargetsReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The subquery is what makes this different from entityTopTargets: both
// directions are collected first and the sum is taken over the union, so an
// entity that both points at and is pointed at by the same partner has those
// weights added rather than ranked separately.
const entityTopPartnersQuery = `SELECT partner, SUM(w) AS total FROM (
 SELECT target AS partner, weight AS w FROM entity_edges
 WHERE LOWER(source) = LOWER($1) AND relation = $2 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
 UNION ALL
 SELECT source AS partner, weight AS w FROM entity_edges
 WHERE LOWER(target) = LOWER($1) AND relation = $2 AND edge_class <> 'semantic'` +
	entityEdgeVisibleProjection + `
) sub GROUP BY partner ORDER BY total DESC LIMIT $3`

// entityTopPartners ranks who an entity is most connected to under one
// relation, in either direction.
func entityTopPartners(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	entity, relation, err := db2contract.DecodeEntityTopPartnersRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityTopPartnersMaxRows
	found, status := readNeighbours(ctx, store, entityTopPartnersQuery,
		[]any{entity, relation, int64(ceiling)}, ceiling)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	converted := make([]db2contract.EntityTopPartnersRow, 0, len(found))
	for _, row := range found {
		converted = append(converted, db2contract.EntityTopPartnersRow(row))
	}
	reply, err := db2contract.EncodeEntityTopPartnersReply(converted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

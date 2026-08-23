package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEntityProfileUpsert,
		db2contract.OperationEntityProfileUpsert, entityProfileUpsert)
	Register(db2contract.StageEntityNodeAliasUpsert,
		db2contract.OperationEntityNodeAliasUpsert, entityNodeAliasUpsert)
	Register(db2contract.StageEntityTopTriples,
		db2contract.OperationEntityTopTriples, entityTopTriples)
}

// created_at is written on insert and deliberately not on update: the conflict
// clause names three columns and leaves the fourth alone, so a profile
// refreshed a hundred times still says when it was first built.
//
// canonical_name is also left alone on conflict, which is the C's choice and a
// deliberate one -- the name a profile is filed under does not change because
// it was observed again, and a refresh that carried a worse name would rewrite
// it.
const entityProfileUpsertQuery = `INSERT INTO entity_profiles
 (entity_id, canonical_name, observation_count, card_json, last_refreshed, created_at)
 VALUES ($1, $2, $3, $4, pg_now_text(), pg_now_text())
 ON CONFLICT (entity_id) DO UPDATE SET
  observation_count = excluded.observation_count,
  card_json = excluded.card_json,
  last_refreshed = excluded.last_refreshed`

// entityProfileUpsert stores what is known about an entity.
//
// An empty canonical name falls back to the identifier. The C does the same,
// and it matters on the insert path only: a profile written with an empty name
// would render as a blank row, whereas the identifier is at least something a
// person can recognise.
func entityProfileUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entityID, canonicalName, observations, cardJSON, err :=
		db2contract.DecodeEntityProfileUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if canonicalName == "" {
		canonicalName = entityID
	}
	_, execErr := store.Exec(ctx, entityProfileUpsertQuery,
		entityID, canonicalName, int64(observations), cardJSON)
	return acknowledgement(execErr == nil,
		db2contract.EncodeEntityProfileUpsertReply)
}

// The conflict target is the pair, not the alias: one alias can point at
// several nodes, and one node can be reached by several aliases. What an
// upsert refreshes is the pairing -- which project last saw it, and in which
// generation.
const entityNodeAliasUpsertQuery = `INSERT INTO entity_node_aliases
 (alias, node_key, alias_kind, project, last_seen_generation_id)
 VALUES ($1, $2, $3, $4, $5)
 ON CONFLICT (alias, node_key) DO UPDATE SET
  alias_kind = excluded.alias_kind,
  project = excluded.project,
  last_seen_generation_id = excluded.last_seen_generation_id`

// entityNodeAliasUpsert records that a name refers to a graph node.
//
// The generation is what makes an alias forgettable: a sweep can drop aliases
// last seen before the current generation without having to know which scan
// wrote them.
//
// node_key is a foreign key into entity_nodes, so an alias for a node that does
// not exist fails rather than dangling.
func entityNodeAliasUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	alias, nodeKey, aliasKind, project, generation, err :=
		db2contract.DecodeEntityNodeAliasUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, entityNodeAliasUpsertQuery,
		alias, nodeKey, aliasKind, project, int64(generation))
	return acknowledgement(execErr == nil,
		db2contract.EncodeEntityNodeAliasUpsertReply)
}

// The zero in the select list is not a placeholder for a missing column: it is
// what makes the DISTINCT mean anything. Selecting the real id would make every
// row distinct by definition, and the same triple recorded by several edges
// would come back once per edge.
//
// weight is selected as well as ordered by, which is what PostgreSQL requires
// of a SELECT DISTINCT -- an ORDER BY over an unselected column is rejected
// outright. Two edges with the same triple and different weights therefore
// still produce two rows; the deduplication is of identical triples, not of
// triples regardless of weight.
const entityTopTriplesQuery = `SELECT DISTINCT 0 AS id, source, relation, target, weight
 FROM entity_edges
 WHERE edge_class <> 'semantic'` + entityEdgeVisibleProjection + `
 ORDER BY weight DESC LIMIT $1`

// entityTopTriples lists the heaviest observed relations in the graph.
//
// Semantic edges are excluded, as everywhere else edges are read: they are
// inferred rather than observed, and mixing them in would let a guess look like
// a recorded fact.
func entityTopTriples(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeEntityTopTriplesRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EntityTopTriplesMaxRows
	rows, queryErr := store.Query(ctx, entityTopTriplesQuery, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	triples := make([]db2contract.EntityTopTriplesRow, 0, 16)
	for rows.Next() {
		var id, weight int64
		var source, relation, target string
		if scanErr := rows.Scan(
			&id, &source, &relation, &target, &weight); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		triples = append(triples, db2contract.EntityTopTriplesRow{
			EdgeID:       uint64(id),
			EdgeWeight:   clampToU32(weight),
			EdgeSource:   source,
			EdgeRelation: relation,
			EdgeTarget:   target,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeEntityTopTriplesReply(triples)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

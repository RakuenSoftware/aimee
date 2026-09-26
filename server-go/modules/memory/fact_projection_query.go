package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
)

func (s *postgresDataStore) RecallFactProjection(ctx context.Context, entity, query string, sensitive bool, capacity int) (string, int, *factProjection, error) {
	if s.placement != PlacementKB || capacity < 1 {
		return "", 0, nil, errors.New("memory: fact projection requires KB placement and positive capacity")
	}
	names := []string{entity}
	if entity == "" {
		sensitive = TurnRequestsSensitive(query)
		mentioned, err := s.mentionedEntities(ctx, query)
		if err != nil {
			return "", 0, nil, err
		}
		names = append([]string{"user"}, mentioned...)
	}
	// Entity discovery remains a separate candidate step. All selected assertions
	// and parents are observed together, instead of one snapshot/RPC per entity.
	rows, err := s.db.Query(ctx, `SELECT n.ordinality,e.relation,e.target,e.confidence,e.id,e.version,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),`+assertionMemoryVersions+`
 FROM unnest($1::text[]) WITH ORDINALITY AS n(entity,ordinality)
 CROSS JOIN LATERAL (
 SELECT e.* FROM entity_edges e WHERE e.source=n.entity AND `+currentFactRecallSQL()+`
 ORDER BY e.confidence DESC,e.id ASC LIMIT $2
 ) e ORDER BY n.ordinality,e.confidence DESC,e.id ASC`, names, factRecallMaxFacts)
	if err != nil {
		return "", 0, nil, err
	}
	defer rows.Close()
	refs := []typedProjectionRef{}
	var block strings.Builder
	var skipped int64
	for rows.Next() {
		var ordinal int64
		var relation, target, parents string
		var confidence float64
		var hit assertionHit
		if err := rows.Scan(&ordinal, &relation, &target, &confidence, &hit.ID, &hit.Version, &hit.ownerID, &parents); err != nil {
			return "", 0, nil, err
		}
		if ordinal == skipped {
			continue
		}
		line := factRecallLine(relation, target, confidence, sensitive)
		if line == "" {
			continue
		}
		// As in legacy recall, stop this entity at its first overflowing line,
		// then allow a smaller line from a later entity to use the remainder.
		if block.Len()+len(line) >= capacity {
			skipped = ordinal
			continue
		}
		if err := json.Unmarshal([]byte(parents), &hit.memoryParents); err != nil {
			return "", 0, nil, err
		}
		hit.StableID, hit.memoryParentsObserved = strconv.FormatInt(hit.ID, 10), true
		for i := range hit.memoryParents {
			hit.memoryParents[i].SchemaVersion, hit.memoryParents[i].OwnerID = 1, hit.ownerID
		}
		ref := typedProjectionRef{Channel: "facts", ID: hit.StableID, Source: hit.sourceVersion()}
		if ref.Source == nil || !validTypedSource(ref) {
			return "", 0, nil, errors.New("memory: fact source versions unavailable or exceed capacity")
		}
		refs = append(refs, ref)
		block.WriteString(line)
	}
	if err := rows.Err(); err != nil {
		return "", 0, nil, err
	}
	text := block.String()
	if !validFactSources(text, refs) {
		return "", 0, nil, errors.New("memory: invalid fact source projection")
	}
	return text, len(refs), newFactProjection(text, refs), nil
}

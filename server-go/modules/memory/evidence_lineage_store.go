package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

type storedLineageRow struct {
	Kind string `json:"kind"`
	Ref  string `json:"ref"`
}
type storedLineageInput struct {
	SchemaVersion   int    `json:"schema_version"`
	Owner           string `json:"owner_id"`
	RecordID        string `json:"record_id"`
	Revision        string `json:"record_revision"`
	DerivedRevision string `json:"derived_revision"`
	Inputs          []struct {
		RecordID string `json:"record_id"`
		Revision string `json:"record_revision"`
	} `json:"inputs"`
}

// Evidence diagnostics do not return copied text. Incomplete or inaccessible
// ancestry withholds all references and counts; missing and hidden inputs are
// intentionally indistinguishable. The enclosing request pins repeatable read.
func (s *postgresDataStore) memoryEvidence(ctx context.Context, id int64) (map[string]any, error) {
	var isolation string
	if err := s.db.QueryRow(ctx, `SHOW transaction_isolation`).Scan(&isolation); err != nil {
		return nil, err
	}
	if isolation != "repeatable read" && isolation != "serializable" {
		return nil, fmt.Errorf("memory: evidence requires a stable snapshot")
	}
	claim := strconv.FormatInt(id, 10)
	nodes := map[string]lineageNode{}
	revisions := map[string]string{}
	origins := map[string]bool{}
	owner := ""
	unavailable := false
	pending := []int64{id}
	for len(pending) > 0 && len(nodes) < 256 {
		next := pending[0]
		pending = pending[1:]
		key := strconv.FormatInt(next, 10)
		if _, ok := nodes[key]; ok {
			continue
		}
		var revision, event, nodeOwner, raw string
		var local bool
		var err error
		if s.placement == PlacementKB {
			err = s.db.QueryRow(ctx, `SELECT m.record_revision::text,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 COALESCE((SELECT min(event_id) FROM memory_evidence_events e WHERE e.object_kind='memory'
 AND e.object_id=m.id::text AND e.operation='assert' HAVING count(*)=1),''),
 (`+currentEpisodeCardInputsSQL("m.", false)+`),
 COALESCE((SELECT jsonb_agg(jsonb_build_object('kind',CASE WHEN octet_length(ref)>8192 THEN 'oversized' ELSE kind END,'ref',CASE WHEN octet_length(ref)>8192 THEN '' ELSE ref END))::text FROM (
 SELECT source_kind AS kind,source_ref AS ref FROM memory_lineage
 WHERE object_type='memory' AND object_id=m.id
 UNION ALL SELECT l.source_kind,l.source_ref FROM memory_units u JOIN memory_lineage l
 ON l.object_type='memory_unit' AND l.object_id=u.id AND l.source_kind='episode-card-input-v1'
 WHERE u.memory_id=m.id AND u.is_episode_card=1 AND u.unit_type='episode_card'
 LIMIT 257) entry),'[]')
 FROM memories m WHERE m.id=$1 AND `+baseCurrentMemorySQL("m."), next).Scan(&revision, &nodeOwner, &event, &local, &raw)
		} else {
			err = s.db.QueryRow(ctx, `SELECT m.record_revision::text,
 (SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1),
 COALESCE((SELECT min(generation)::text FROM user_memory_invalidation_outbox e
 WHERE e.memory_id=m.id AND e.operation='insert' HAVING count(*)=1),''),true,'[]'
 FROM user_memories m WHERE m.id=$1 AND `+personalCurrentMemorySQL("m."), next).Scan(&revision, &nodeOwner, &event, &local, &raw)
		}
		if store.IsNoRows(err) {
			if next == id {
				return nil, ErrMemoryNotFound
			}
			unavailable = true
			nodes[key] = lineageNode{}
			continue
		}
		if err != nil {
			return nil, err
		}
		if owner != "" && owner != nodeOwner {
			return nil, fmt.Errorf("memory: lineage owner changed")
		}
		owner = nodeOwner
		node := lineageNode{Revision: revision, Complete: local}
		revisions[key] = revision
		var rows []storedLineageRow
		if json.Unmarshal([]byte(raw), &rows) != nil {
			return nil, fmt.Errorf("memory: invalid stored lineage")
		}
		if len(rows) > 256 {
			unavailable = true
			node.Complete = false
			rows = nil
		}
		legacy := map[string]bool{}
		declared := map[string]bool{}
		producer := false
		for _, row := range rows {
			switch row.Kind {
			case "memory":
				legacy[strings.TrimPrefix(row.Ref, "memory:")] = true
			case derivedIndexSource:
				// Self-ownership of the deterministic index is not a parent.
				if row.Ref != key {
					node.Complete = false
				}
			case "metadata":
				producer = producer || strings.HasPrefix(row.Ref, "memory-cognify-v1:")
			case "memory-cognify-input-v1", "episode-card-input-v1":
				var input storedLineageInput
				if json.Unmarshal([]byte(row.Ref), &input) != nil {
					return nil, fmt.Errorf("memory: invalid producer observation")
				}
				if input.SchemaVersion != 1 || input.Owner != owner {
					node.Complete = false
					continue
				}
				if row.Kind == "memory-cognify-input-v1" {
					if input.DerivedRevision != revision {
						node.Complete = false
					}
					input.Inputs = append(input.Inputs, struct {
						RecordID string `json:"record_id"`
						Revision string `json:"record_revision"`
					}{input.RecordID, input.Revision})
				}
				if len(input.Inputs) > 256 {
					node.Complete = false
					continue
				}
				for _, parent := range input.Inputs {
					parentID, err := strconv.ParseInt(parent.RecordID, 10, 64)
					if err != nil || parentID <= 0 || strconv.FormatInt(parentID, 10) != parent.RecordID {
						node.Complete = false
						continue
					}
					declared[parent.RecordID] = true
					node.Edges = append(node.Edges, lineageEdge{Parent: parent.RecordID, Revision: parent.Revision, Claim: claim, Relation: "derived_from"})
					if len(pending) >= 4096 {
						node.Complete = false
						break
					}
					pending = append(pending, parentID)
				}
			default:
				// Unimplemented source types cannot certify complete ancestry.
				node.Complete = false
			}
		}
		for parent := range legacy {
			if !declared[parent] {
				node.Complete = false
			}
		}
		if producer && len(declared) == 0 {
			node.Complete = false
		}
		if len(declared) == 0 && len(legacy) == 0 && !producer && event != "" {
			family := fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(owner+"\x00"+event)))
			node.Origin = &lineageOrigin{Family: family}
			origins[family] = true
		}
		if !node.Complete {
			unavailable = true
		}
		nodes[key] = node
	}
	if len(pending) > 0 {
		unavailable = true
	}
	projection := projectLineage(claim, []string{claim}, nodes, 256, 16)
	result := map[string]any{"schema_version": 1, "record_id": claim, "owner_id": owner,
		"support_count": projection.SupportCount, "source_family_count": projection.SourceFamilyCount,
		"independent_support_count": projection.IndependentSupportCount, "independence_state": projection.IndependenceState,
		"unknown_origin_count": projection.UnknownOriginCount, "lineage_state": projection.LineageState,
		"lineage_generation": nil, "generation_state": "unavailable", "reasons": projection.Reasons,
		"evidence": []map[string]string{}, "origin_families": []string{}}
	if unavailable {
		for _, field := range []string{"support_count", "source_family_count", "independent_support_count", "unknown_origin_count"} {
			result[field] = nil
		}
		result["lineage_state"], result["independence_state"] = "partial", "unknown"
		result["reasons"] = []string{"dependencies_unavailable"}
		return result, nil
	}
	var generation string
	generationSQL := `SELECT ` + collectionRevisionSQL
	if s.placement == PlacementServer {
		generationSQL = `SELECT (generation+1)::text FROM user_memory_collection_generation WHERE id=1`
	}
	if err := s.db.QueryRow(ctx, generationSQL).Scan(&generation); err != nil {
		return nil, err
	}
	if n, err := strconv.ParseInt(generation, 10, 64); err != nil || n < 1 {
		return nil, fmt.Errorf("memory: invalid lineage generation")
	}
	result["lineage_generation"], result["generation_state"] = json.Number(generation), "observed"
	keys := make([]string, 0, len(revisions))
	for key := range revisions {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	refs := []map[string]string{}
	for _, key := range keys {
		refs = append(refs, map[string]string{"record_id": key, "record_revision": revisions[key]})
	}
	families := []string{}
	for family := range origins {
		families = append(families, family)
	}
	sort.Strings(families)
	result["evidence"], result["origin_families"] = refs, families
	return result, nil
}

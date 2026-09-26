package memory

import (
	"context"
	"encoding/json"
	"sort"
)

// These are the retired C unit-semantic policy's boosts and admission floors.
// They affect similarity only; eligibility and parent deduplication happen
// before a candidate can consume the unit channel's result budget.
type unitSemanticPolicy struct {
	Types     map[string]float64 `json:"types"`
	Kinds     map[string]float64 `json:"kinds"`
	Floors    map[string]float64 `json:"floors"`
	OtherKind float64            `json:"other_kind"`
}

func semanticUnitPolicy(intent string) unitSemanticPolicy {
	p := unitSemanticPolicy{Types: map[string]float64{}, Kinds: map[string]float64{}, Floors: map[string]float64{}}
	switch intent {
	case "temporal":
		p.Types = map[string]float64{"temporal": .18, "event": .10, "summary": -.03}
		p.Kinds = map[string]float64{"episodic": .18, "semantic": -.06}
		p.Floors = map[string]float64{"temporal": .46, "event": .46, "summary": .58}
	case "entity":
		p.Types = map[string]float64{"entity": .18, "event": .08}
		p.Kinds = map[string]float64{"semantic": .12, "episodic": -.03}
	case "procedural":
		p.Types = map[string]float64{"summary": .08, "chunk": .08}
		p.Kinds = map[string]float64{"procedural": .22}
		p.OtherKind = -.08
	default:
		p.Types = map[string]float64{"event": .05, "summary": .03}
		p.Kinds = map[string]float64{"semantic": .12, "episodic": -.03}
	}
	return p
}

type semanticCandidate struct {
	record Record
	score  float64
	lanes  uint16
}

func (s *postgresDataStore) unitSemanticCandidates(ctx context.Context, req DataRequest, exact bool, version, vector string, scale float64) ([]semanticCandidate, error) {
	policy, _ := json.Marshal(semanticUnitPolicy(answerIntent(req.Query)))
	rows, err := s.db.Query(ctx, embeddingInputs()+`, candidates AS (
 SELECT i.memory_id,i.unit_type,
 CASE WHEN vector_dims(v.embedding)=vector_dims($10::vector) AND vector_norm(v.embedding)>0
 THEN 1-(v.embedding <=> $10::vector) END
 +i.unit_weight*0.03+COALESCE(($12::jsonb->'types'->>i.unit_type)::double precision,0)
 +COALESCE(($12::jsonb->'kinds'->>COALESCE(NULLIF(i.unit_kind,''),'episodic'))::double precision,($12::jsonb->>'other_kind')::double precision) AS similarity,
 COALESCE(($12::jsonb->'floors'->>i.unit_type)::double precision,0.52)*$13 AS floor
 FROM inputs i JOIN memory_embedding_versions v ON v.version=$9 AND v.point_id=i.point_id AND v.input_hash=i.input_hash
 JOIN memories m ON m.id=i.memory_id
 WHERE i.record_type='unit' AND `+currentMemorySQL("m.")+`
 AND CASE WHEN $1 THEN m.scope_type=$2 AND m.scope_value=$3
 ELSE $4 OR m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared')
 OR (m.scope_type='project' AND m.scope_value=$5) OR (m.scope_type='workspace' AND m.scope_value=$6) END
 AND ($7='' OR m.kind=$7) AND ($8='' OR m.tier=$8)
 AND vector_dims(v.embedding)=vector_dims($10::vector)
 AND i.unit_weight>'-Infinity'::double precision AND i.unit_weight<'Infinity'::double precision
 ), parents AS (
 SELECT memory_id,max(similarity)*1.05 AS similarity,bool_or(unit_type='temporal') AS temporal
 FROM candidates WHERE similarity>=floor GROUP BY memory_id
 ) SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence,p.similarity,p.temporal
 FROM parents p JOIN memories m ON m.id=p.memory_id ORDER BY CASE
 WHEN $1 THEN 0 WHEN m.scope_type='project' AND m.scope_value=$5 THEN 0
 WHEN m.scope_type='workspace' AND m.scope_value=$6 THEN 1
 WHEN m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared') THEN 2 ELSE 3 END,
 p.similarity DESC,m.id LIMIT $11`, append(graphScopeArgs(req, exact), version, vector, min(req.Limit, 256), string(policy), scale)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []semanticCandidate{}
	for rows.Next() {
		var c semanticCandidate
		var temporal bool
		r := &c.record
		if err := rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &c.score, &temporal); err != nil {
			return nil, err
		}
		c.lanes = laneUnit | laneSemantic
		if temporal {
			c.lanes |= laneTemporal
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// A parent enters semantic fusion once, using its strongest admitted whole-row
// or unit signal. Multiple units are not independent votes for that parent.
func mergeSemanticCandidates(req DataRequest, exact bool, whole, units []semanticCandidate) []Record {
	byID := map[int64]semanticCandidate{}
	for _, channel := range [][]semanticCandidate{whole, units} {
		for _, c := range channel {
			req.lanes.add([]Record{c.record}, c.lanes)
			previous, exists := byID[c.record.ID]
			if !exists || c.score > previous.score {
				byID[c.record.ID] = c
			}
		}
	}
	all := make([]semanticCandidate, 0, len(byID))
	for _, c := range byID {
		all = append(all, c)
	}
	sort.Slice(all, func(i, j int) bool {
		if !exact {
			left, right := recallScopeRank(all[i].record, req), recallScopeRank(all[j].record, req)
			if left != right {
				return left < right
			}
		}
		if all[i].score != all[j].score {
			return all[i].score > all[j].score
		}
		return all[i].record.ID < all[j].record.ID
	})
	out := make([]Record, 0, min(len(all), req.Limit))
	for _, c := range all[:min(len(all), req.Limit)] {
		out = append(out, c.record)
	}
	return out
}

package memory

import (
	"context"
	"math"
	"sort"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

// Shared semantic recall uses the active version's original inputs and serving
// identity. A stale vector cannot become a candidate merely because its parent
// ID still exists. Scope and lifecycle filters precede the candidate cap.
func (s *postgresDataStore) fuseSharedSemantic(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	if s.placement != PlacementKB || s.recallExecutor == nil || req.Query == "" || req.Limit <= 0 {
		return base, nil
	}
	// The data owner pins each request to a transaction. Keep version selection,
	// query embedding and the candidate read under the same rebuild lock.
	if _, ok := s.db.(store.Tx); !ok {
		return base, nil
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock_shared($1)`, vectorRebuildLock); err != nil {
		return nil, err
	}
	version, command, dimension, err := s.activeEmbeddingVersion(ctx)
	if err != nil {
		return nil, err
	}
	if version == "" || command == "" {
		return base, nil
	}
	var identity string
	if err := s.db.QueryRow(ctx, `SELECT serving_id FROM memory_embedder_versions WHERE version=$1`, version).Scan(&identity); err != nil {
		return nil, err
	}
	if identity == "" {
		return base, nil
	}
	scale := sharedSemanticFloorScale(dimension, 0)
	if s.settings != nil {
		settings, err := s.settings()
		if err != nil {
			return base, nil
		}
		scale = sharedSemanticFloorScale(dimension, configNumber(settings, "memory_semantic_floor_scale"))
	}
	// Dependency failure keeps the lexical result usable. Only network/model
	// work gets the short budget; cancelling SQL in the request transaction
	// would also invalidate the lexical result's commit.
	budget := 1500 * time.Millisecond
	if deadline, ok := ctx.Deadline(); ok {
		budget = min(budget, time.Until(deadline)/2)
	}
	embedCtx, cancel := context.WithTimeout(ctx, budget)
	defer cancel()
	before := EmbedServingID(embedCtx, 0, s.recallExecutor, command)
	if before.Error != "" || before.ServingID != identity {
		return base, nil
	}
	query := Embed(embedCtx, 0, s.recallExecutor, EmbedRequest{BaseURL: command, Text: req.Query, InputType: "query", MaxDim: dimension})
	if query.Error != "" || query.Unavailable || query.Unauthorized || query.Truncated || len(query.Vector) != dimension {
		return base, nil
	}
	vector, err := vectorLiteral(query.Vector)
	if err != nil {
		return base, nil
	}
	after := EmbedServingID(embedCtx, 0, s.recallExecutor, command)
	if after.Error != "" || after.ServingID != identity {
		return base, nil
	}
	// Fingerprints cover text, scope and kind, so moved/edited records need fresh
	// embeddings. Unit recall has additional intent/temporal weights; this channel
	// restores the whole-record candidates and leaves those weights separate.
	rows, err := s.db.Query(ctx, embeddingInputs+`, candidates AS (
 SELECT i.memory_id, CASE WHEN vector_dims(v.embedding)=vector_dims($10::vector) AND vector_norm(v.embedding)>0
 THEN 1-(v.embedding <=> $10::vector) END AS similarity
 FROM inputs i JOIN memory_embedding_versions v
 ON v.version=$9 AND v.point_id=i.point_id AND v.input_hash=i.input_hash
 JOIN memories m ON m.id=i.memory_id
 WHERE i.record_type='memory' AND m.lifecycle_state='active' AND m.activation_suppressed=0
 AND CASE WHEN $1 THEN m.scope_type=$2 AND m.scope_value=$3
 ELSE $4 OR m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared')
 OR (m.scope_type='project' AND m.scope_value=$5) OR (m.scope_type='workspace' AND m.scope_value=$6) END
 AND ($7='' OR m.kind=$7) AND ($8='' OR m.tier=$8)
 AND vector_dims(v.embedding)=vector_dims($10::vector)
 ) SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence
 FROM candidates c JOIN memories m ON m.id=c.memory_id
 WHERE c.similarity >= $12 ORDER BY CASE
 WHEN $1 THEN 0 WHEN m.scope_type='project' AND m.scope_value=$5 THEN 0
 WHEN m.scope_type='workspace' AND m.scope_value=$6 THEN 1
 WHEN m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared') THEN 2 ELSE 3 END,
 c.similarity DESC,m.id LIMIT $11`,
		append(graphScopeArgs(req, exact), version, vector, min(req.Limit, 256), .62*scale)...)
	if err != nil {
		return nil, err
	}
	semantic, err := scanRecordRows(rows)
	if err != nil {
		return nil, err
	}
	combined := fusePersonal(base, semantic, len(base)+len(semantic))
	if !exact {
		scopeRank := func(r Record) int {
			switch {
			case r.Scope.Type == ScopeProject && r.Scope.Value == req.Project:
				return 0
			case r.Scope.Type == ScopeWorkspace && r.Scope.Value == req.Workspace:
				return 1
			case r.Scope.Type == ScopeGlobal || (r.Scope.Type == ScopeWorkspace && r.Scope.Value == "_shared"):
				return 2
			default:
				return 3
			}
		}
		sort.SliceStable(combined, func(i, j int) bool { return scopeRank(combined[i]) < scopeRank(combined[j]) })
	}
	return combined[:min(len(combined), req.Limit)], nil
}

func sharedSemanticFloorScale(dimension int, configured float64) float64 {
	if configured > 0 && !math.IsNaN(configured) && !math.IsInf(configured, 0) {
		return configured
	}
	switch {
	case dimension <= 384:
		return 1
	case dimension <= 1024:
		return .55
	case dimension <= 2560:
		return .75
	default:
		return .65
	}
}

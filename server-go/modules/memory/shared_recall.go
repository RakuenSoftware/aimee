package memory

import (
	"context"
	"errors"
	"fmt"
	"math"
	"sort"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

// Shared semantic recall uses the active version's original inputs and serving
// identity. A stale vector cannot become a candidate merely because its parent
// ID still exists. Scope and lifecycle filters precede the candidate cap.
func (s *postgresDataStore) fuseSharedSemantic(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	if s.placement != PlacementKB || req.Query == "" || req.Limit <= 0 {
		return base, nil
	}
	if s.recallExecutor == nil && !s.requireSemantic {
		recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "unavailable", Reason: "shared_embedder_not_configured"})
		return base, nil
	}
	unavailable := func() ([]Record, error) {
		recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "unavailable", Reason: "bounded_embedding_or_index_fallback", Quota: min(req.Limit, 256)})
		if s.requireSemantic {
			return nil, errors.New("memory: evaluation semantic recall unavailable")
		}
		return base, nil
	}
	// The data owner pins each request to a transaction. Keep version selection,
	// query embedding and the candidate read under the same rebuild lock.
	if _, ok := s.db.(store.Tx); !ok {
		return unavailable()
	}
	var present bool
	if err := s.db.QueryRow(ctx, `WITH locked AS MATERIALIZED (
 SELECT pg_advisory_xact_lock_shared($1)
) SELECT to_regclass('memory_embedder_versions') IS NOT NULL FROM locked`, vectorRebuildLock).Scan(&present); err != nil {
		return nil, err
	}
	if !present {
		return unavailable()
	}
	// Read the catalog in a new statement after acquiring the lock: a cutover
	// that held the exclusive lock must be visible in this statement's snapshot.
	var version, command, identity string
	var dimension int
	err := s.db.QueryRow(ctx, `SELECT v.version,v.command,v.dimension,v.serving_id
 FROM memory_active_embedder a JOIN memory_embedder_versions v ON v.version=a.version
 WHERE a.id=1`).Scan(&version, &command, &dimension, &identity)
	if store.IsNoRows(err) {
		return unavailable()
	}
	if err != nil {
		return nil, err
	}
	if version == "" || command == "" || (EmbedIsHTTP(command) && s.recallExecutor == nil) {
		return unavailable()
	}
	if identity == "" {
		return unavailable()
	}
	scale := sharedSemanticFloorScale(dimension, 0)
	if s.settings != nil {
		settings, err := s.settings()
		if err != nil {
			return unavailable()
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
	before, err := versionServingIdentity(embedCtx, 0, s.recallExecutor, command)
	if err != nil || before != identity {
		return unavailable()
	}
	query := Embed(embedCtx, 0, s.recallExecutor, EmbedRequest{BaseURL: command, Text: req.Query, InputType: "query", MaxDim: dimension})
	if query.Error != "" || query.Unavailable || query.Unauthorized || query.Truncated || len(query.Vector) != dimension {
		return unavailable()
	}
	vector, err := vectorLiteral(query.Vector)
	if err != nil {
		return unavailable()
	}
	after, err := versionServingIdentity(embedCtx, 0, s.recallExecutor, command)
	if err != nil || after != identity {
		return unavailable()
	}
	// Fingerprints cover text, scope and kind, so moved/edited records need fresh
	// embeddings. Whole-record and unit channels each get an eligible-parent budget.
	rows, err := s.db.Query(ctx, embeddingInputs+`, candidates AS (
 SELECT i.memory_id, CASE WHEN vector_dims(v.embedding)=vector_dims($10::vector) AND vector_norm(v.embedding)>0
 THEN 1-(v.embedding <=> $10::vector) END AS similarity
 FROM inputs i JOIN memory_embedding_versions v
 ON v.version=$9 AND v.point_id=i.point_id AND v.input_hash=i.input_hash
 JOIN memories m ON m.id=i.memory_id
 WHERE i.record_type='memory' AND `+currentMemorySQL("m.")+`
 AND CASE WHEN $1 THEN m.scope_type=$2 AND m.scope_value=$3
 ELSE $4 OR m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared')
 OR (m.scope_type='project' AND m.scope_value=$5) OR (m.scope_type='workspace' AND m.scope_value=$6) END
 AND ($7='' OR m.kind=$7) AND ($8='' OR m.tier=$8)
 AND vector_dims(v.embedding)=vector_dims($10::vector)
 ) SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence,c.similarity,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),m.record_revision::text
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
	whole := []semanticCandidate{}
	for rows.Next() {
		c := semanticCandidate{lanes: laneSemantic}
		r := &c.record
		r.observedVersion = &MemoryRecordVersion{SchemaVersion: 1}
		if err := rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &c.score, &r.observedVersion.OwnerID, &r.observedVersion.RecordRevision); err != nil {
			rows.Close()
			return nil, err
		}
		r.observedVersion.RecordID = fmt.Sprint(r.ID)
		recordNativeRank(ctx, r, "semantic", len(whole)+1, c.score, "cosine_similarity_scope_priority")
		whole = append(whole, c)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	units, err := s.unitSemanticCandidates(ctx, req, exact, version, vector, scale)
	if err != nil {
		return nil, err
	}
	semantic := mergeSemanticCandidates(req, exact, whole, units)
	combined := fuseRanked(ctx, base, semantic, len(base)+len(semantic), "prior_candidates", "semantic_parent")
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
	recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "available", Reason: "active_version_and_serving_identity_verified", Candidates: len(semantic), Quota: min(req.Limit, 256), IndexVersion: version, IndexReadiness: "eligible_versions_only; coverage_not_proven"})
	return combined, nil
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

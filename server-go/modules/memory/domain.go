package memory

import (
	"context"
	"errors"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

type MemoryLink struct {
	ID        int64   `json:"id"`
	SourceID  int64   `json:"source_id"`
	TargetID  int64   `json:"target_id"`
	Relation  string  `json:"relation"`
	Weight    float64 `json:"weight"`
	CreatedAt string  `json:"created_at"`
}

type Provenance struct {
	ID        int64  `json:"id"`
	MemoryID  int64  `json:"memory_id"`
	SessionID string `json:"session_id"`
	Action    string `json:"action"`
	Details   string `json:"details"`
	CreatedAt string `json:"created_at"`
}

type Conflict struct {
	ID         int64  `json:"id"`
	MemoryAID  int64  `json:"memory_a_id"`
	MemoryBID  int64  `json:"memory_b_id"`
	DetectedAt string `json:"detected_at"`
	Resolved   bool   `json:"resolved"`
	Resolution string `json:"resolution"`
}

type ScopeTag struct {
	Type  string `json:"type"`
	Value string `json:"value"`
}

type ScopeRank struct {
	ID   int64 `json:"id"`
	Rank int   `json:"rank"`
}

type CodeStats struct {
	Projects    int `json:"projects"`
	Files       int `json:"files"`
	Definitions int `json:"definitions"`
	Embeddings  int `json:"embeddings"`
}

type MemoryStats struct {
	GraphFusionEnabled bool            `json:"graph_fusion_enabled"`
	CodeIndex          *CodeStats      `json:"code_index,omitempty"`
	TierKinds          []TierKindCount `json:"tier_kinds"`
	TierCounts         map[string]int  `json:"tier_counts"`
	KindCounts         map[string]int  `json:"kind_counts"`
	Total              int             `json:"total"`
	Conflicts          int             `json:"conflicts"`
}

type MemoryHealth struct {
	ContradictionRate float64 `json:"contradiction_rate"`
	PromotionRate     float64 `json:"promotion_rate"`
	DemotionRate      float64 `json:"demotion_rate"`
	Staleness         float64 `json:"staleness"`
	Contradictions    int     `json:"total_contradictions"`
	Promotions        int     `json:"total_promotions"`
	Demotions         int     `json:"total_demotions"`
	Expirations       int     `json:"total_expirations"`
	Cycles            int     `json:"cycles"`
}

type LifecycleCounts struct {
	Active     int `json:"active"`
	Pending    int `json:"pending"`
	Fulfilled  int `json:"fulfilled"`
	Superseded int `json:"superseded"`
	Archived   int `json:"archived"`
}

type Episode struct {
	ID            int64  `json:"id"`
	MemoryID      int64  `json:"memory_id"`
	Key           string `json:"episode_key"`
	Text          string `json:"episode_text"`
	SourceSession string `json:"source_session"`
	ReferenceTime string `json:"reference_time"`
	CreatedAt     string `json:"created_at"`
}

type Relation struct {
	ID        int64   `json:"id"`
	MemoryID  int64   `json:"memory_id"`
	EpisodeID int64   `json:"episode_id"`
	Source    string  `json:"source"`
	Relation  string  `json:"relation"`
	Target    string  `json:"target"`
	Fact      string  `json:"fact"`
	ValidAt   string  `json:"valid_at"`
	InvalidAt string  `json:"invalid_at"`
	Weight    float64 `json:"weight"`
	CreatedAt string  `json:"created_at"`
}

type EntityProfile struct {
	Entity        string `json:"entity"`
	Mentions      int    `json:"mention_count"`
	Relations     int    `json:"relation_count"`
	LatestEpisode string `json:"latest_episode"`
	Summary       string `json:"summary"`
}

func (s *postgresDataStore) requireKBDomain() error {
	if s.placement != PlacementKB {
		return errors.New("memory: kb memory operation used in server placement")
	}
	return nil
}

func (s *postgresDataStore) Touch(ctx context.Context, ids []int64) (int, error) {
	if err := s.requireKBDomain(); err != nil {
		return 0, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memories SET use_count=use_count+1,
last_used_at=pg_now_text(), updated_at=pg_now_text() WHERE id=ANY($1::text::bigint[])`, memoryIDsParameter(ids))
	if err != nil {
		return 0, err
	}
	return int(tag.RowsAffected()), nil
}

// Legacy content edits use the canonical model transition and return its new
// identity. They cannot mutate old content or bypass epistemic/author checks.
func (s *postgresDataStore) UpdateContent(ctx context.Context, id int64, content string) (int64, error) {
	code, next, err := s.UpdateAs(ctx, id, content, AuthorityModel)
	if err != nil {
		return 0, err
	}
	switch code {
	case MutationImmutableExperience:
		return 0, errImmutableExperience
	case MutationRequiresReplacement:
		return 0, errRequiresRevocation
	case MutationReviewRequired:
		return 0, errMutationReviewRequired
	}
	return next, nil
}

func (s *postgresDataStore) Reject(ctx context.Context, id int64, reason string) (out bool, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "reject", ID: id}, DataResponse{Updated: out}, err, "")
	}()
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	var changed int
	err = s.db.QueryRow(ctx, `WITH target AS (
 SELECT key,content,scope_type,scope_value FROM memories WHERE id=$1
), tomb AS (
 INSERT INTO memory_rejection_tombstones(object_kind,memory_key,memory_content,scope_type,scope_value,reason)
 SELECT 'memory',key,content,scope_type,scope_value,$2 FROM target
 ON CONFLICT (memory_key,memory_content,scope_type,scope_value) WHERE object_kind='memory' AND active=1
 DO UPDATE SET reason=EXCLUDED.reason,rejected_at=pg_now_text()
 RETURNING 1
), archived AS (
 UPDATE memories SET lifecycle_state='archived',archive_reason=$2,activation_suppressed=1,
 updated_at=pg_now_text() WHERE id=$1 AND EXISTS(SELECT 1 FROM tomb) RETURNING 1
) SELECT COUNT(*) FROM archived`, id, reason).Scan(&changed)
	return changed > 0, err
}

func scanLink(row store.Row, item *MemoryLink) error {
	return row.Scan(&item.ID, &item.SourceID, &item.TargetID, &item.Relation, &item.Weight, &item.CreatedAt)
}

func (s *postgresDataStore) LinkCreate(ctx context.Context, sourceID, targetID int64, relation string) (MemoryLink, error) {
	if err := s.requireKBDomain(); err != nil {
		return MemoryLink{}, err
	}
	var item MemoryLink
	err := scanLink(s.db.QueryRow(ctx, `INSERT INTO memory_links(source_id,target_id,relation)
SELECT $1,$2,$3 WHERE $1 IN (SELECT id FROM memories) AND $2 IN (SELECT id FROM memories)
RETURNING id,source_id,target_id,relation,weight,created_at`, sourceID, targetID, relation), &item)
	return item, err
}

func (s *postgresDataStore) LinkQuery(ctx context.Context, id int64, limit int) ([]MemoryLink, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,source_id,target_id,relation,weight,created_at
FROM memory_links WHERE (source_id=$1 OR target_id=$1)
AND source_id IN (SELECT id FROM memories) AND target_id IN (SELECT id FROM memories) ORDER BY created_at DESC,id DESC LIMIT $2`, id, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]MemoryLink, 0)
	for rows.Next() {
		var item MemoryLink
		if err := rows.Scan(&item.ID, &item.SourceID, &item.TargetID, &item.Relation, &item.Weight, &item.CreatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) LinkDelete(ctx context.Context, id int64) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `DELETE FROM memory_links WHERE id=$1
AND source_id IN (SELECT id FROM memories) AND target_id IN (SELECT id FROM memories)`, id)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) ProvenanceList(ctx context.Context, id int64, limit int) ([]Provenance, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,memory_id,session_id,action,COALESCE(details,''),created_at
FROM memory_provenance WHERE memory_id=$1 AND memory_id IN (SELECT id FROM memories) ORDER BY id LIMIT $2`, id, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Provenance, 0)
	for rows.Next() {
		var item Provenance
		if err := rows.Scan(&item.ID, &item.MemoryID, &item.SessionID, &item.Action, &item.Details, &item.CreatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) ProvenanceAdd(ctx context.Context, id int64, session, action, details string) (Provenance, error) {
	if err := s.requireKBDomain(); err != nil {
		return Provenance{}, err
	}
	var item Provenance
	err := s.db.QueryRow(ctx, `INSERT INTO memory_provenance(memory_id,session_id,action,details,created_at)
VALUES($1,$2,$3,$4,pg_now_text()) RETURNING id,memory_id,session_id,action,COALESCE(details,''),created_at`,
		id, session, action, details).Scan(&item.ID, &item.MemoryID, &item.SessionID, &item.Action, &item.Details, &item.CreatedAt)
	return item, err
}

func scanConflict(row store.Row, item *Conflict) error {
	var resolved int
	err := row.Scan(&item.ID, &item.MemoryAID, &item.MemoryBID, &item.DetectedAt, &resolved, &item.Resolution)
	item.Resolved = resolved != 0
	return err
}

func (s *postgresDataStore) ConflictList(ctx context.Context, limit int) ([]Conflict, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,memory_a,memory_b,detected_at,resolved,COALESCE(resolution,'')
FROM memory_conflicts WHERE resolved=0
AND memory_a IN (SELECT id FROM memories) AND memory_b IN (SELECT id FROM memories) ORDER BY detected_at DESC,id DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Conflict, 0)
	for rows.Next() {
		var item Conflict
		var resolved int
		if err := rows.Scan(&item.ID, &item.MemoryAID, &item.MemoryBID, &item.DetectedAt, &resolved, &item.Resolution); err != nil {
			return nil, err
		}
		item.Resolved = resolved != 0
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) ConflictRecord(ctx context.Context, a, b int64) (Conflict, error) {
	if err := s.requireKBDomain(); err != nil {
		return Conflict{}, err
	}
	var item Conflict
	err := scanConflict(s.db.QueryRow(ctx, `INSERT INTO memory_conflicts(memory_a,memory_b,detected_at)
SELECT $1,$2,pg_now_text() WHERE NOT EXISTS(SELECT 1 FROM memory_conflicts
 WHERE resolved=0 AND ((memory_a=$1 AND memory_b=$2) OR (memory_a=$2 AND memory_b=$1)))
RETURNING id,memory_a,memory_b,detected_at,resolved,COALESCE(resolution,'')`, a, b), &item)
	if store.IsNoRows(err) {
		err = scanConflict(s.db.QueryRow(ctx, `SELECT id,memory_a,memory_b,detected_at,resolved,COALESCE(resolution,'')
FROM memory_conflicts WHERE resolved=0 AND ((memory_a=$1 AND memory_b=$2) OR (memory_a=$2 AND memory_b=$1))
ORDER BY id DESC LIMIT 1`, a, b), &item)
	}
	return item, err
}

func (s *postgresDataStore) ConflictResolve(ctx context.Context, id int64, resolution string) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memory_conflicts SET resolved=1,resolution=$2 WHERE id=$1 AND resolved=0`, id, resolution)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) ScopeTag(ctx context.Context, id int64, scope Scope) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	scope, err := normalizeScope(PlacementKB, scope)
	if err != nil {
		return false, err
	}
	// One statement keeps canonical ownership and compatibility projections
	// atomic, including callers already inside a larger transaction.
	var updated bool
	err = s.db.QueryRow(ctx, `WITH changed AS (
 UPDATE memories SET scope_type=$2,scope_value=$3,updated_at=pg_now_text() WHERE id=$1 RETURNING id
), tags AS (
 INSERT INTO memory_scopes(memory_id,scope_type,scope_value)
 SELECT id,$2,$3 FROM changed WHERE true ON CONFLICT DO NOTHING
), workspaces AS (
 INSERT INTO memory_workspaces(memory_id,workspace)
 SELECT id,$3 FROM changed WHERE $2='workspace' ON CONFLICT DO NOTHING
) SELECT EXISTS(SELECT 1 FROM changed)`, id, scope.Type, scope.Value).Scan(&updated)
	return updated, err
}

func (s *postgresDataStore) ScopeCollect(ctx context.Context, id int64) ([]ScopeTag, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT scope_type,scope_value FROM (
 SELECT scope_type,scope_value,0 AS ordering FROM memories WHERE id=$1
 UNION ALL SELECT scope_type,scope_value,1 FROM memory_scopes WHERE memory_id=$1
 AND memory_id IN (SELECT id FROM memories)
) s GROUP BY scope_type,scope_value ORDER BY MIN(ordering),scope_type,scope_value`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]ScopeTag, 0)
	for rows.Next() {
		var item ScopeTag
		if err := rows.Scan(&item.Type, &item.Value); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) PrimaryScope(ctx context.Context, id int64) (ScopeTag, error) {
	tags, err := s.ScopeCollect(ctx, id)
	if err != nil {
		return ScopeTag{}, err
	}
	// Collection puts the canonical row first. Historical tags describe
	// provenance; they cannot override current ownership after a scope change.
	if len(tags) == 0 {
		return ScopeTag{}, nil
	}
	return tags[0], nil
}

func (s *postgresDataStore) ScopeRanks(ctx context.Context, ids []int64, workspace, project string, includeAll bool) ([]ScopeRank, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,CASE
 WHEN $4 THEN 1 WHEN $3<>'' AND scope_type='project' AND scope_value=$3 THEN 3
 WHEN $2<>'' AND scope_type='workspace' AND scope_value=$2 THEN 2
 WHEN (scope_type='global' AND scope_value='_global') OR
      (scope_type='workspace' AND scope_value='_shared') THEN 1 ELSE 0 END
FROM memories WHERE id=ANY($1::text::bigint[])`, memoryIDsParameter(ids), workspace, project, includeAll)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	byID := make(map[int64]int, len(ids))
	for rows.Next() {
		var id int64
		var rank int
		if err := rows.Scan(&id, &rank); err != nil {
			return nil, err
		}
		byID[id] = rank
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	result := make([]ScopeRank, 0, len(ids))
	for _, id := range ids {
		result = append(result, ScopeRank{ID: id, Rank: byID[id]})
	}
	return result, nil
}

func (s *postgresDataStore) Stats(ctx context.Context) (MemoryStats, error) {
	result := MemoryStats{GraphFusionEnabled: s.graphFusionEnabled(), TierKinds: []TierKindCount{}, TierCounts: map[string]int{}, KindCounts: map[string]int{}}
	table := "memories"
	if s.placement == PlacementServer {
		table = "user_memories"
	}
	rows, err := s.db.Query(ctx, `SELECT tier,kind,COUNT(*) FROM `+table+` GROUP BY tier,kind ORDER BY tier,kind`)
	if err != nil {
		return result, err
	}
	for rows.Next() {
		var item TierKindCount
		if err := rows.Scan(&item.Tier, &item.Kind, &item.Count); err != nil {
			rows.Close()
			return result, err
		}
		result.TierKinds = append(result.TierKinds, item)
		result.TierCounts[item.Tier] += item.Count
		result.KindCounts[item.Kind] += item.Count
		result.Total += item.Count
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return result, err
	}

	if s.placement == PlacementServer {
		var present bool
		if err := s.db.QueryRow(ctx, `SELECT to_regclass('user_code_files') IS NOT NULL`).Scan(&present); err != nil {
			return result, err
		}
		code := CodeStats{}
		result.CodeIndex = &code
		if present {
			err = s.db.QueryRow(ctx, `SELECT count(DISTINCT project),count(*),COALESCE(sum(jsonb_array_length(definitions)),0),
count(*) FILTER(WHERE embedding IS NOT NULL AND embedding_fingerprint=fingerprint) FROM user_code_files`).Scan(&code.Projects, &code.Files, &code.Definitions, &code.Embeddings)
			if err != nil {
				return result, err
			}
		}
	}
	if s.placement == PlacementServer {
		return result, nil
	}
	err = s.db.QueryRow(ctx, `SELECT COUNT(*) FROM memory_conflicts WHERE resolved=0
AND memory_a IN (SELECT id FROM memories) AND memory_b IN (SELECT id FROM memories)`).Scan(&result.Conflicts)
	return result, err
}

func ratio(numerator, denominator int) float64 {
	if denominator <= 0 {
		return 0
	}
	return float64(numerator) / float64(denominator)
}

func (s *postgresDataStore) Health(ctx context.Context) (MemoryHealth, error) {
	var result MemoryHealth
	if err := s.requireKBDomain(); err != nil {
		return result, err
	}
	var newMemories, eligible, l2Total, stale int
	err := s.db.QueryRow(ctx, `SELECT COUNT(*),COALESCE(SUM(contradictions_detected),0)::bigint,
COALESCE(SUM(promotions),0)::bigint,COALESCE(SUM(demotions),0)::bigint,
COALESCE(SUM(expirations),0)::bigint
FROM memory_health WHERE cycle_at>=pg_now_text('-7 days')`).Scan(&result.Cycles, &result.Contradictions,
		&result.Promotions, &result.Demotions, &result.Expirations)
	if err != nil {
		return result, err
	}
	err = s.db.QueryRow(ctx, `SELECT
COUNT(*) FILTER(WHERE created_at>=pg_now_text('-7 days')),
COUNT(*) FILTER(WHERE tier='L1' AND (use_count>=3 OR confidence>=0.8)),
COUNT(*) FILTER(WHERE tier='L2'),
COUNT(*) FILTER(WHERE tier='L2' AND (last_used_at IS NULL OR last_used_at<pg_now_text('-30 days')))
FROM memories`).Scan(&newMemories, &eligible, &l2Total, &stale)
	if err != nil {
		return result, err
	}
	result.ContradictionRate = ratio(result.Contradictions, newMemories)
	result.PromotionRate = ratio(result.Promotions, eligible*result.Cycles)
	result.DemotionRate = ratio(result.Demotions, l2Total*result.Cycles)
	result.Staleness = ratio(stale, l2Total)
	return result, nil
}

func validLifecycle(state string) bool {
	switch state {
	case "active", "pending", "fulfilled", "superseded", "archived":
		return true
	}
	return false
}

func (s *postgresDataStore) LifecycleGet(ctx context.Context, id int64) (string, error) {
	if err := s.requireKBDomain(); err != nil {
		return "", err
	}
	var state string
	err := s.db.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=$1`, id).Scan(&state)
	if store.IsNoRows(err) {
		return "", ErrMemoryNotFound
	}
	return state, err
}

func (s *postgresDataStore) LifecycleTransition(ctx context.Context, id int64, state, reason string) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	if !validLifecycle(state) {
		return false, errors.New("memory: invalid lifecycle state")
	}
	tag, err := s.db.Exec(ctx, `UPDATE memories SET lifecycle_state=$2,archive_reason=$3,
updated_at=pg_now_text() WHERE id=$1`, id, state, reason)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) LifecyclePending(ctx context.Context, id int64, ttlDays int) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memories SET lifecycle_state='pending',
ttl_at=pg_now_text(($2::text||' days')::text),updated_at=pg_now_text()
WHERE id=$1 AND lifecycle_state='active'`, id, ttlDays)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) LifecycleSweep(ctx context.Context) (int, error) {
	if err := s.requireKBDomain(); err != nil {
		return 0, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memories SET lifecycle_state='archived',
archive_reason='pending ttl expired',updated_at=pg_now_text()
WHERE lifecycle_state='pending' AND ttl_at<>'' AND ttl_at<pg_now_text()`)
	if err != nil {
		return 0, err
	}
	return int(tag.RowsAffected()), nil
}

func (s *postgresDataStore) LifecycleCounts(ctx context.Context) (LifecycleCounts, error) {
	var result LifecycleCounts
	if err := s.requireKBDomain(); err != nil {
		return result, err
	}
	err := s.db.QueryRow(ctx, `SELECT COUNT(*) FILTER(WHERE lifecycle_state='active'),
COUNT(*) FILTER(WHERE lifecycle_state='pending'),COUNT(*) FILTER(WHERE lifecycle_state='fulfilled'),
COUNT(*) FILTER(WHERE lifecycle_state='superseded'),COUNT(*) FILTER(WHERE lifecycle_state='archived')
FROM memories`).Scan(&result.Active, &result.Pending, &result.Fulfilled, &result.Superseded, &result.Archived)
	return result, err
}

func (s *postgresDataStore) FactHistory(ctx context.Context, key string, limit int) ([]Record, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+queryRecordColumns+`
FROM memories WHERE (key=$1 OR starts_with(key,$1||'#v')) AND `+historicalMemoryInspectionSQL("")+` ORDER BY created_at DESC,id DESC LIMIT $2`, key, limit)
	if err != nil {
		return nil, err
	}
	records, err := scanRecordRows(rows, false)
	for i := range records {
		records[i].historicalRead = true
	}
	return records, err
}

func normalizeMatchText(value string) string { return strings.ToLower(strings.TrimSpace(value)) }

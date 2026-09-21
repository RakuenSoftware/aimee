package memory

import (
	"context"
	"errors"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
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

type MemoryStats struct {
	TierCounts map[string]int `json:"tier_counts"`
	KindCounts map[string]int `json:"kind_counts"`
	Total      int            `json:"total"`
	Conflicts  int            `json:"conflicts"`
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
last_used_at=pg_now_text(), updated_at=pg_now_text() WHERE id=ANY($1)`, ids)
	if err != nil {
		return 0, err
	}
	return int(tag.RowsAffected()), nil
}

func (s *postgresDataStore) UpdateContent(ctx context.Context, id int64, content string) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memories SET content=$2, updated_at=pg_now_text()
WHERE id=$1 AND lifecycle_state='active'`, id, content)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) Reject(ctx context.Context, id int64, reason string) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	var changed int
	err := s.db.QueryRow(ctx, `WITH target AS (
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
VALUES($1,$2,$3) RETURNING id,source_id,target_id,relation,weight,created_at`, sourceID, targetID, relation), &item)
	return item, err
}

func (s *postgresDataStore) LinkQuery(ctx context.Context, id int64, limit int) ([]MemoryLink, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,source_id,target_id,relation,weight,created_at
FROM memory_links WHERE source_id=$1 OR target_id=$1 ORDER BY created_at DESC,id DESC LIMIT $2`, id, limit)
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
	tag, err := s.db.Exec(ctx, `DELETE FROM memory_links WHERE id=$1`, id)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) ProvenanceList(ctx context.Context, id int64, limit int) ([]Provenance, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,memory_id,session_id,action,COALESCE(details,''),created_at
FROM memory_provenance WHERE memory_id=$1 ORDER BY id LIMIT $2`, id, limit)
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
FROM memory_conflicts WHERE resolved=0 ORDER BY detected_at DESC,id DESC LIMIT $1`, limit)
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
	tag, err := s.db.Exec(ctx, `UPDATE memories SET scope_type=$2,scope_value=$3,updated_at=pg_now_text() WHERE id=$1`, id, scope.Type, scope.Value)
	if err != nil || tag.RowsAffected() == 0 {
		return false, err
	}
	_, err = s.db.Exec(ctx, `INSERT INTO memory_scopes(memory_id,scope_type,scope_value)
VALUES($1,$2,$3) ON CONFLICT DO NOTHING`, id, scope.Type, scope.Value)
	return err == nil, err
}

func (s *postgresDataStore) ScopeCollect(ctx context.Context, id int64) ([]ScopeTag, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT scope_type,scope_value FROM (
 SELECT scope_type,scope_value,0 AS ordering FROM memories WHERE id=$1
 UNION SELECT scope_type,scope_value,1 FROM memory_scopes WHERE memory_id=$1
) s ORDER BY ordering,scope_type,scope_value`, id)
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
	rank := func(kind string) int {
		switch kind {
		case "project":
			return 3
		case "workspace":
			return 2
		case "global":
			return 1
		default:
			return 0
		}
	}
	primary := ScopeTag{}
	for _, tag := range tags {
		if rank(tag.Type) > rank(primary.Type) {
			primary = tag
		}
	}
	return primary, nil
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
FROM memories WHERE id=ANY($1)`, ids, workspace, project, includeAll)
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
	result := MemoryStats{TierCounts: map[string]int{}, KindCounts: map[string]int{}}
	if err := s.requireKBDomain(); err != nil {
		return result, err
	}
	rows, err := s.db.Query(ctx, `SELECT tier,COUNT(*) FROM memories GROUP BY tier`)
	if err != nil {
		return result, err
	}
	for rows.Next() {
		var name string
		var count int
		if err := rows.Scan(&name, &count); err != nil {
			rows.Close()
			return result, err
		}
		result.TierCounts[name] = count
		result.Total += count
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return result, err
	}
	rows.Close()
	rows, err = s.db.Query(ctx, `SELECT kind,COUNT(*) FROM memories GROUP BY kind`)
	if err != nil {
		return result, err
	}
	defer rows.Close()
	for rows.Next() {
		var name string
		var count int
		if err := rows.Scan(&name, &count); err != nil {
			return result, err
		}
		result.KindCounts[name] = count
	}
	if err := rows.Err(); err != nil {
		return result, err
	}
	err = s.db.QueryRow(ctx, `SELECT COUNT(*) FROM memory_conflicts WHERE resolved=0`).Scan(&result.Conflicts)
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

const episodeColumns = `id,memory_id,episode_key,episode_text,source_session,reference_time,created_at`

func scanEpisode(row store.Row, item *Episode) error {
	return row.Scan(&item.ID, &item.MemoryID, &item.Key, &item.Text, &item.SourceSession, &item.ReferenceTime, &item.CreatedAt)
}

func (s *postgresDataStore) EpisodeList(ctx context.Context, query string, limit int) ([]Episode, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+episodeColumns+` FROM memory_episodes
WHERE $1='' OR episode_key ILIKE '%'||$1||'%' OR episode_text ILIKE '%'||$1||'%'
ORDER BY reference_time DESC,created_at DESC,id DESC LIMIT $2`, query, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Episode, 0)
	for rows.Next() {
		var item Episode
		if err := rows.Scan(&item.ID, &item.MemoryID, &item.Key, &item.Text, &item.SourceSession, &item.ReferenceTime, &item.CreatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) EpisodeGet(ctx context.Context, key string) (Episode, error) {
	if err := s.requireKBDomain(); err != nil {
		return Episode{}, err
	}
	var item Episode
	err := scanEpisode(s.db.QueryRow(ctx, `SELECT `+episodeColumns+` FROM memory_episodes
WHERE episode_key=$1 ORDER BY id DESC LIMIT 1`, key), &item)
	if store.IsNoRows(err) {
		return Episode{}, ErrMemoryNotFound
	}
	return item, err
}

const relationColumns = `id,memory_id,COALESCE(episode_id,0),src_entity,relation,dst_entity,
fact_text,valid_at,invalid_at,weight,created_at`

func scanRelationRows(rows store.Rows) ([]Relation, error) {
	defer rows.Close()
	items := make([]Relation, 0)
	for rows.Next() {
		var item Relation
		if err := rows.Scan(&item.ID, &item.MemoryID, &item.EpisodeID, &item.Source, &item.Relation,
			&item.Target, &item.Fact, &item.ValidAt, &item.InvalidAt, &item.Weight, &item.CreatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) RelationSearch(ctx context.Context, query, asOf string, limit int) ([]Relation, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+relationColumns+` FROM memory_relations
WHERE ($1='' OR src_entity ILIKE '%'||$1||'%' OR relation ILIKE '%'||$1||'%' OR
dst_entity ILIKE '%'||$1||'%' OR fact_text ILIKE '%'||$1||'%')
AND ($2='' OR ((valid_at='' OR valid_at<=$2) AND (invalid_at='' OR invalid_at>$2)))
ORDER BY weight DESC,CASE WHEN valid_at<>'' THEN 1 ELSE 0 END DESC,created_at DESC LIMIT $3`, query, asOf, limit)
	if err != nil {
		return nil, err
	}
	return scanRelationRows(rows)
}

func (s *postgresDataStore) EntityEdges(ctx context.Context, entity string, limit int) ([]Relation, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+relationColumns+` FROM memory_relations
WHERE lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)
ORDER BY weight DESC,created_at DESC LIMIT $2`, entity, limit)
	if err != nil {
		return nil, err
	}
	return scanRelationRows(rows)
}

func (s *postgresDataStore) EntityProfile(ctx context.Context, entity string) (EntityProfile, error) {
	result := EntityProfile{Entity: entity}
	if err := s.requireKBDomain(); err != nil {
		return result, err
	}
	err := s.db.QueryRow(ctx, `SELECT
(SELECT COUNT(*) FROM memory_relations WHERE lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)),
(SELECT COUNT(DISTINCT relation) FROM memory_relations WHERE lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)),
COALESCE((SELECT me.episode_key FROM memory_episodes me JOIN memory_relations mr ON mr.episode_id=me.id
 WHERE lower(mr.src_entity)=lower($1) OR lower(mr.dst_entity)=lower($1)
 ORDER BY me.reference_time DESC,me.created_at DESC LIMIT 1),''),
COALESCE((SELECT string_agg(fact_text,'; ' ORDER BY weight DESC) FROM
 (SELECT DISTINCT fact_text,weight FROM memory_relations WHERE fact_text<>'' AND
  (lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)) ORDER BY weight DESC LIMIT 8) facts),'')`,
		entity).Scan(&result.Mentions, &result.Relations, &result.LatestEpisode, &result.Summary)
	return result, err
}

func (s *postgresDataStore) FactHistory(ctx context.Context, key string, limit int) ([]Record, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM memories WHERE key=$1 OR key LIKE $1||'#v%' ORDER BY created_at DESC,id DESC LIMIT $2`, key, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Record, 0)
	for rows.Next() {
		var item Record
		if err := rows.Scan(&item.ID, &item.Scope.Type, &item.Scope.Value, &item.Tier,
			&item.Kind, &item.Key, &item.Content, &item.Confidence); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func normalizeMatchText(value string) string { return strings.ToLower(strings.TrimSpace(value)) }

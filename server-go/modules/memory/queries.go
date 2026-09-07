package memory

import (
	"context"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

type LowEffectiveness struct {
	ID            int64   `json:"id"`
	Tier          string  `json:"tier"`
	Kind          string  `json:"kind"`
	Key           string  `json:"key"`
	Effectiveness float64 `json:"effectiveness"`
	UseCount      int     `json:"use_count"`
}

type SupersededKey struct {
	BaseKey  string `json:"base_key"`
	Versions int    `json:"versions"`
}

type ReviewRecord struct {
	ID             int64   `json:"id"`
	Tier           string  `json:"tier"`
	Kind           string  `json:"kind"`
	Key            string  `json:"key"`
	Content        string  `json:"content"`
	Confidence     float64 `json:"confidence"`
	LifecycleState string  `json:"lifecycle_state"`
	ReviewReason   string  `json:"review_reason"`
	ScopeType      string  `json:"scope_type"`
	ScopeValue     string  `json:"scope_value"`
	CreatedAt      string  `json:"created_at"`
	UpdatedAt      string  `json:"updated_at"`
}

type MemorySummary struct {
	Scope   string `json:"scope"`
	Summary string `json:"summary"`
}

type MemoryScene struct {
	ID          int64  `json:"id"`
	WorkspaceID string `json:"workspace_id"`
	TurnCount   int    `json:"turn_count"`
	CreatedAt   string `json:"created_at"`
}

type SceneMember struct {
	MemoryID           int64   `json:"memory_id"`
	Key                string  `json:"key"`
	MembershipStrength float64 `json:"membership_strength"`
}

type TierKindCount struct {
	Tier  string `json:"tier"`
	Kind  string `json:"kind"`
	Count int    `json:"count"`
}

func (s *postgresDataStore) KeyExists(ctx context.Context, key string) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	var exists bool
	err := s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memories WHERE key=$1)`, key).Scan(&exists)
	return exists, err
}

func (s *postgresDataStore) FindID(ctx context.Context, key, kind string) (int64, error) {
	if err := s.requireKBDomain(); err != nil {
		return 0, err
	}
	var id int64
	err := s.db.QueryRow(ctx, `SELECT id FROM memories WHERE key=$1 AND ($2='' OR kind=$2)
ORDER BY id LIMIT 1`, key, kind).Scan(&id)
	if store.IsNoRows(err) {
		return 0, nil
	}
	return id, err
}

func scanRecordRows(rows store.Rows) ([]Record, error) {
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

const queryRecordColumns = `id,scope_type,scope_value,tier,kind,key,content,confidence`

func (s *postgresDataStore) QueryRecords(ctx context.Context, mode, pattern string, days, limit int) ([]Record, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	var rows store.Rows
	var err error
	switch mode {
	case "like":
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE lifecycle_state='active' AND (key ILIKE '%'||$1||'%' OR content ILIKE '%'||$1||'%')
ORDER BY CASE WHEN lower(key)=lower($1) THEN 0 WHEN lower(content)=lower($1) THEN 1
 WHEN lower(key) LIKE lower($1)||'%' THEN 2 ELSE 3 END,tier DESC,use_count DESC LIMIT $2`, pattern, limit)
	case "top-l2":
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE lifecycle_state='active' AND tier='L2' AND kind='fact'
ORDER BY use_count DESC,confidence DESC,id DESC LIMIT $1`, limit)
	case "session-priority":
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE lifecycle_state='active' AND tier IN ('L1','L2','L3') AND
($1='' OR key ILIKE $1 OR content ILIKE $1)
ORDER BY CASE kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,
CASE tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,use_count DESC,id DESC LIMIT $2`, pattern, limit)
	case "facts-patterns":
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE lifecycle_state='active' AND tier IN ('L2','L3','L5') AND kind IN ('fact','pattern')
AND (key ILIKE '%'||$1||'%' OR content ILIKE '%'||$1||'%')
ORDER BY confidence DESC,use_count DESC,id DESC LIMIT $2`, pattern, limit)
	case "eval":
		rows, err = s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE lifecycle_state='active' AND tier IN ('L1','L2','L3') AND kind<>'scratch'
ORDER BY CASE WHEN tier='L2' AND kind='fact' THEN 0 WHEN kind='fact' THEN 1 ELSE 2 END,
confidence DESC,id DESC LIMIT $1`, limit)
	default:
		return nil, errors.New("memory: invalid query-records mode")
	}
	if err != nil {
		return nil, err
	}
	return scanRecordRows(rows)
}

func (s *postgresDataStore) LowEffectiveness(ctx context.Context, threshold float64, limit int) ([]LowEffectiveness, error) {
	rows, err := s.db.Query(ctx, `SELECT id,tier,kind,key,effectiveness,use_count FROM memories
WHERE effectiveness IS NOT NULL AND effectiveness<$1 ORDER BY effectiveness,id LIMIT $2`, threshold, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]LowEffectiveness, 0)
	for rows.Next() {
		var item LowEffectiveness
		if err := rows.Scan(&item.ID, &item.Tier, &item.Kind, &item.Key, &item.Effectiveness, &item.UseCount); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) UnusedL2(ctx context.Context, days, limit int) ([]Record, error) {
	if days <= 0 {
		days = 30
	}
	rows, err := s.db.Query(ctx, `SELECT `+queryRecordColumns+` FROM memories
WHERE tier='L2' AND use_count=0 AND created_at<pg_now_text(('-'||$1::text||' days')::text)
ORDER BY created_at,id LIMIT $2`, days, limit)
	if err != nil {
		return nil, err
	}
	return scanRecordRows(rows)
}

func (s *postgresDataStore) SupersededKeys(ctx context.Context, minVersions, limit int) ([]SupersededKey, error) {
	if minVersions <= 0 {
		minVersions = 2
	}
	rows, err := s.db.Query(ctx, `SELECT regexp_replace(key,'#v[0-9]+$','') AS base_key,COUNT(*)
FROM memories WHERE key~'#v[0-9]+$' GROUP BY base_key HAVING COUNT(*)>=$1
ORDER BY COUNT(*) DESC,base_key LIMIT $2`, minVersions, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]SupersededKey, 0)
	for rows.Next() {
		var item SupersededKey
		if err := rows.Scan(&item.BaseKey, &item.Versions); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) ReviewList(ctx context.Context, state string, limit int) ([]ReviewRecord, error) {
	query := `SELECT id,tier,kind,key,content,confidence,lifecycle_state,
COALESCE(NULLIF(archive_reason,''),(SELECT reason FROM memory_rejection_tombstones t
 WHERE t.object_kind='memory' AND t.memory_key=m.key AND t.memory_content=m.content
 ORDER BY t.id DESC LIMIT 1),''),scope_type,scope_value,created_at,updated_at
FROM memories m WHERE ($1='' OR lifecycle_state=$1 OR ($1='rejected' AND EXISTS(
 SELECT 1 FROM memory_rejection_tombstones t WHERE t.object_kind='memory' AND t.active=1
 AND t.memory_key=m.key AND t.memory_content=m.content AND t.scope_type=m.scope_type AND t.scope_value=m.scope_value)))
ORDER BY updated_at DESC,id DESC LIMIT $2`
	if s.placement == PlacementServer {
		query = `SELECT id,tier,kind,key,content,confidence,lifecycle_state,
'', 'user', '_user', created_at::text, updated_at::text
FROM user_memories WHERE ($1='' OR lifecycle_state=$1)
ORDER BY updated_at DESC,id DESC LIMIT $2`
	}
	rows, err := s.db.Query(ctx, query, state, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]ReviewRecord, 0)
	for rows.Next() {
		var item ReviewRecord
		if err := rows.Scan(&item.ID, &item.Tier, &item.Kind, &item.Key, &item.Content,
			&item.Confidence, &item.LifecycleState, &item.ReviewReason, &item.ScopeType,
			&item.ScopeValue, &item.CreatedAt, &item.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) Restore(ctx context.Context, id int64, actor string) (bool, error) {
	var restored int
	err := s.db.QueryRow(ctx, `WITH target AS (
 SELECT key,content,scope_type,scope_value FROM memories WHERE id=$1
), tomb AS (
 UPDATE memory_rejection_tombstones t SET active=0,restored_at=pg_now_text(),restored_by=$2
 FROM target x WHERE t.object_kind='memory' AND t.active=1 AND t.memory_key=x.key
 AND t.memory_content=x.content AND t.scope_type=x.scope_type AND t.scope_value=x.scope_value
 AND (t.rejected_by='' OR t.rejected_by=$2) RETURNING 1
), changed AS (
 UPDATE memories SET lifecycle_state='active',activation_suppressed=0,archive_reason='',updated_at=pg_now_text()
 WHERE id=$1 AND EXISTS(SELECT 1 FROM tomb) RETURNING 1
) SELECT COUNT(*) FROM changed`, id, actor).Scan(&restored)
	return restored > 0, err
}

func (s *postgresDataStore) SetArtifact(ctx context.Context, id int64, kind, ref, hash string) (bool, error) {
	tag, err := s.db.Exec(ctx, `UPDATE memories SET artifact_type=$2,artifact_ref=$3,artifact_hash=$4,
updated_at=pg_now_text() WHERE id=$1`, id, kind, ref, hash)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) Summaries(ctx context.Context, id int64, limit int) ([]MemorySummary, error) {
	rows, err := s.db.Query(ctx, `SELECT scope,summary FROM memory_summaries
WHERE memory_id=$1 ORDER BY id LIMIT $2`, id, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]MemorySummary, 0)
	for rows.Next() {
		var item MemorySummary
		if err := rows.Scan(&item.Scope, &item.Summary); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) Scenes(ctx context.Context, limit int) ([]MemoryScene, error) {
	rows, err := s.db.Query(ctx, `SELECT id,workspace_id,turn_count,created_at FROM memory_scenes
ORDER BY created_at DESC,id DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]MemoryScene, 0)
	for rows.Next() {
		var item MemoryScene
		if err := rows.Scan(&item.ID, &item.WorkspaceID, &item.TurnCount, &item.CreatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) SceneMembers(ctx context.Context, sceneID int64, limit int) ([]SceneMember, error) {
	rows, err := s.db.Query(ctx, `SELECT sm.memory_id,m.key,sm.membership_strength
FROM memory_scene_members sm JOIN memories m ON m.id=sm.memory_id WHERE sm.scene_id=$1
ORDER BY sm.membership_strength DESC,sm.memory_id LIMIT $2`, sceneID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]SceneMember, 0)
	for rows.Next() {
		var item SceneMember
		if err := rows.Scan(&item.MemoryID, &item.Key, &item.MembershipStrength); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) AllIDs(ctx context.Context) ([]int64, error) {
	rows, err := s.db.Query(ctx, `SELECT id FROM memories ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]int64, 0)
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		items = append(items, id)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) EpistemicKind(ctx context.Context, id int64) (string, error) {
	var kind string
	err := s.db.QueryRow(ctx, `SELECT epistemic_kind FROM memories WHERE id=$1`, id).Scan(&kind)
	if store.IsNoRows(err) {
		return "", ErrMemoryNotFound
	}
	return kind, err
}

func (s *postgresDataStore) DemoteConfidence(ctx context.Context, id int64) (bool, error) {
	tag, err := s.db.Exec(ctx, `UPDATE memories SET confidence=GREATEST(confidence-0.1,0.0),
updated_at=pg_now_text() WHERE id=$1`, id)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) TierKindCounts(ctx context.Context, limit int) ([]TierKindCount, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT tier,kind,COUNT(*) FROM memories
GROUP BY tier,kind ORDER BY tier,kind LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]TierKindCount, 0)
	for rows.Next() {
		var item TierKindCount
		if err := rows.Scan(&item.Tier, &item.Kind, &item.Count); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

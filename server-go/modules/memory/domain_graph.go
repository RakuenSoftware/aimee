package memory

import (
	"context"
	store "github.com/JBailes/aimee/server-go/db"
)

// Scope is pinned by handleData on the transaction. Parent RLS is an additional
// bound; currentness applies even for all-scope and privileged reads.
const domainScopeRankSQL = `CASE
 WHEN current_setting('aimee.memory_scope_all',true)='1' THEN 1
 WHEN scope_type='project' AND scope_value=current_setting('aimee.memory_project',true) THEN 3
 WHEN scope_type='workspace' AND scope_value=current_setting('aimee.memory_workspace',true) THEN 2
 ELSE 1 END`

var domainVisibleParents = `SELECT id,` + domainScopeRankSQL + ` AS scope_rank FROM memories
 WHERE ` + currentMemorySQL("")

const episodeColumns = `id,memory_id,episode_key,episode_text,source_session,reference_time,created_at`

func scanEpisode(row store.Row, item *Episode) error {
	return row.Scan(&item.ID, &item.MemoryID, &item.Key, &item.Text, &item.SourceSession, &item.ReferenceTime, &item.CreatedAt)
}

func (s *postgresDataStore) EpisodeList(ctx context.Context, query string, limit int) ([]Episode, error) {
	return s.episodeList(ctx, query, limit, Scope{})
}
func (s *postgresDataStore) episodeList(ctx context.Context, query string, limit int, exact Scope) ([]Episode, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `WITH visible AS (`+domainVisibleParents+` AND ($3='' OR (scope_type=$3 AND scope_value=$4))) SELECT `+episodeColumns+` FROM memory_episodes
WHERE ($1='' OR episode_key ILIKE '%'||$1||'%' OR episode_text ILIKE '%'||$1||'%')
AND memory_id IN (SELECT id FROM visible)
ORDER BY (SELECT scope_rank FROM visible WHERE id=memory_id) DESC,reference_time DESC,created_at DESC,id DESC LIMIT $2`, query, limit, exact.Type, exact.Value)
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
	err := scanEpisode(s.db.QueryRow(ctx, `WITH visible AS (`+domainVisibleParents+`) SELECT `+episodeColumns+` FROM memory_episodes
WHERE episode_key=$1 AND memory_id IN (SELECT id FROM visible) ORDER BY id DESC LIMIT 1`, key), &item)
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
	rows, err := s.db.Query(ctx, `WITH visible AS (`+domainVisibleParents+`) SELECT `+relationColumns+` FROM memory_relations
WHERE ($1='' OR src_entity ILIKE '%'||$1||'%' OR relation ILIKE '%'||$1||'%' OR
dst_entity ILIKE '%'||$1||'%' OR fact_text ILIKE '%'||$1||'%')
AND memory_id IN (SELECT id FROM visible)
AND ($2='' OR ((valid_at='' OR valid_at<=$2) AND (invalid_at='' OR invalid_at>$2)))
ORDER BY (SELECT scope_rank FROM visible WHERE id=memory_id) DESC,weight DESC,CASE WHEN valid_at<>'' THEN 1 ELSE 0 END DESC,created_at DESC LIMIT $3`, query, asOf, limit)
	if err != nil {
		return nil, err
	}
	return scanRelationRows(rows)
}

func (s *postgresDataStore) EntityEdges(ctx context.Context, entity string, limit int) ([]Relation, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `WITH visible AS (`+domainVisibleParents+`) SELECT `+relationColumns+` FROM memory_relations
WHERE (lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1))
AND memory_id IN (SELECT id FROM visible)
ORDER BY (SELECT scope_rank FROM visible WHERE id=memory_id) DESC,weight DESC,created_at DESC LIMIT $2`, entity, limit)
	if err != nil {
		return nil, err
	}
	return scanRelationRows(rows)
}

func (s *postgresDataStore) EntityProfile(ctx context.Context, entity string) (EntityProfile, error) {
	return s.entityProfile(ctx, entity, Scope{})
}
func (s *postgresDataStore) entityProfile(ctx context.Context, entity string, exact Scope) (EntityProfile, error) {
	result := EntityProfile{Entity: entity}
	if err := s.requireKBDomain(); err != nil {
		return result, err
	}
	err := s.db.QueryRow(ctx, `WITH visible AS (`+domainVisibleParents+` AND ($2='' OR (scope_type=$2 AND scope_value=$3))),
visible_relations AS (SELECT * FROM memory_relations WHERE memory_id IN (SELECT id FROM visible))
SELECT
(SELECT COUNT(DISTINCT memory_id) FROM memory_entities
 WHERE lower(entity)=lower($1) AND memory_id IN (SELECT id FROM visible)),
(SELECT COUNT(*) FROM visible_relations WHERE lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)) +
(SELECT COUNT(*) FROM entity_edges e WHERE (lower(source)=lower($1) OR lower(target)=lower($1))
 AND edge_class='semantic' AND lifecycle_state IN ('persistent','promoted')
 AND suppressed=0 AND superseded_at='' AND invalidated_at=''
 AND `+memoryValiditySQL("e.")+`
 AND `+currentMemoryEvidenceSQL("e", `$2='' OR (m.scope_type=$2 AND m.scope_value=$3)`, false)+`
 AND (NOT EXISTS(SELECT 1 FROM fact_evidence f WHERE f.assertion_id=e.id AND f.source_kind='memory')
 OR EXISTS(SELECT 1 FROM fact_evidence f WHERE f.assertion_id=e.id AND f.source_kind='memory'
 AND f.invalidated_at='' AND f.stance='supports'))),
COALESCE((SELECT me.episode_key FROM memory_episodes me JOIN visible_relations mr ON mr.episode_id=me.id
 WHERE (lower(mr.src_entity)=lower($1) OR lower(mr.dst_entity)=lower($1))
 AND me.memory_id IN (SELECT id FROM visible)
 ORDER BY (SELECT scope_rank FROM visible WHERE id=me.memory_id) DESC,me.created_at DESC,me.id DESC LIMIT 1),''),
COALESCE((SELECT fact_text FROM visible_relations
 WHERE lower(src_entity)=lower($1) OR lower(dst_entity)=lower($1)
 ORDER BY (SELECT scope_rank FROM visible WHERE id=memory_id) DESC,weight DESC,created_at DESC,id DESC LIMIT 1),'')`,
		entity, exact.Type, exact.Value).Scan(&result.Mentions, &result.Relations, &result.LatestEpisode, &result.Summary)
	if err == nil && result.Mentions == 0 && result.Relations == 0 {
		return result, ErrMemoryNotFound
	}

	return result, err
}

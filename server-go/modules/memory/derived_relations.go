package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
)

const derivedIndexSource = "memory-index-v1"

type derivedGraphRelation struct {
	Source, Relation, Target, Fact, Valid, Invalid string
	Weight                                         float64
	InputID, InputRevision, InputLinkID            int64
}

// The legacy rebuild deleted every relation, including authored and cognified
// facts. Record ownership in the existing lineage ledger instead, so subsequent
// rebuilds replace only this generator's rows and retain stable episode IDs.
func (s *postgresDataStore) markDerivedIndexObject(ctx context.Context, kind string, object, parent int64) error {
	_, err := s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT $1,$2,$3,$4 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage
 WHERE object_type=$1 AND object_id=$2 AND source_kind=$3 AND source_ref=$4)`, kind, object, derivedIndexSource, fmt.Sprint(parent))
	return err
}

func (s *postgresDataStore) replaceDerivedRelations(ctx context.Context, id int64) error {
	if err := s.adoptLegacyDerivedRelations(ctx, id); err != nil {
		return err
	}

	var key, content, session, valid, invalid, primary string
	var parentRevision int64
	if err := s.db.QueryRow(ctx, `SELECT key,content,COALESCE(source_session,''),
 COALESCE(NULLIF(valid_from,''),(SELECT ref_key FROM memory_temporal_refs WHERE memory_id=m.id
 AND granularity IN ('absolute_day','date_phrase','year') ORDER BY weight DESC,id LIMIT 1),created_at),
 COALESCE(valid_until,''),COALESCE((SELECT entity FROM memory_entities WHERE memory_id=m.id
 ORDER BY CASE role WHEN 'actor' THEN 0 WHEN 'subject' THEN 1 WHEN 'person' THEN 2 ELSE 3 END,weight DESC,id LIMIT 1),key),record_revision FROM memories m WHERE id=$1`, id).Scan(&key, &content, &session, &valid, &invalid, &primary, &parentRevision); err != nil {
		return err
	}
	episodes := []struct{ key, text string }{{key, content}}
	rows, err := s.db.Query(ctx, `SELECT scope,summary FROM memory_summaries WHERE memory_id=$1 ORDER BY id LIMIT 2`, id)
	if err != nil {
		return err
	}
	for rows.Next() {
		var k, text string
		if err = rows.Scan(&k, &text); err != nil {
			rows.Close()
			return err
		}
		episodes = append(episodes, struct{ key, text string }{k, text})
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	episodeIDs := []int64{}
	var parentEpisode int64
	for i, e := range episodes {
		if e.text == "" {
			continue
		}
		var episodeID int64
		if err = s.db.QueryRow(ctx, `WITH existing AS (
 SELECT id FROM memory_episodes WHERE memory_id=$1 AND episode_key=$2 AND episode_text=$3 AND source_session=$4 ORDER BY id LIMIT 1
), refreshed AS (UPDATE memory_episodes SET reference_time=$5 WHERE id IN(SELECT id FROM existing) RETURNING id),
 inserted AS (INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session,reference_time)
 SELECT $1,$2,$3,$4,$5 WHERE NOT EXISTS(SELECT 1 FROM existing) RETURNING id)
 SELECT id FROM refreshed UNION ALL SELECT id FROM inserted`, id, e.key, e.text, session, valid).Scan(&episodeID); err != nil {
			return err
		}
		if err = s.markDerivedIndexObject(ctx, "episode", episodeID, id); err != nil {
			return err
		}
		episodeIDs = append(episodeIDs, episodeID)
		if i == 0 {
			parentEpisode = episodeID
		}
	}
	relations := []derivedGraphRelation{}
	rows, err = s.db.Query(ctx, `SELECT actor,action,object,location,event_time FROM memory_event_frames WHERE memory_id=$1 ORDER BY id LIMIT 64`, id)
	if err != nil {
		return err
	}
	for rows.Next() {
		var f derivedFrame
		if err = rows.Scan(&f.Actor, &f.Action, &f.Object, &f.Location, &f.Time); err != nil {
			rows.Close()
			return err
		}
		fact := strings.Join([]string{f.Actor, f.Action, f.Object, f.Location, f.Time}, " ")
		at := valid
		if f.Time != "" {
			at = f.Time
		}
		if f.Actor != "" && f.Action != "" && f.Object != "" {
			relations = append(relations, derivedGraphRelation{f.Actor, f.Action, f.Object, fact, at, invalid, 2.4, 0, 0, 0})
		}
		if f.Actor != "" && f.Location != "" {
			relations = append(relations, derivedGraphRelation{f.Actor, "located_at", f.Location, fact, at, invalid, 1.8, 0, 0, 0})
		}
		if f.Actor != "" && f.Time != "" {
			relations = append(relations, derivedGraphRelation{f.Actor, "occurred_at", f.Time, fact, f.Time, invalid, 1.9, 0, 0, 0})
		}
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	rows, err = s.db.Query(ctx, `SELECT l.relation,target.key,target.content,target.id,target.record_revision,l.id FROM memory_links l
 JOIN memories target ON target.id=l.target_id JOIN memories source ON source.id=l.source_id
 WHERE l.source_id=$1 AND `+currentMemorySQL("target.")+` AND
 (target.scope_type='global' OR (target.scope_type=source.scope_type AND target.scope_value=source.scope_value))
 ORDER BY l.id LIMIT 64`, id)
	if err != nil {
		return err
	}
	for rows.Next() {
		var relation, targetKey, targetContent string
		var targetID, targetRevision, linkID int64
		if err = rows.Scan(&relation, &targetKey, &targetContent, &targetID, &targetRevision, &linkID); err != nil {
			rows.Close()
			return err
		}
		if relation == "" {
			relation = "related_to"
		}
		target, fact := targetKey, targetContent
		if target == "" {
			target = targetContent
		}
		if fact == "" {
			fact = targetKey
		}
		relations = append(relations, derivedGraphRelation{primary, relation, target, primary + " " + relation + " " + fact, valid, invalid, 1.3, targetID, targetRevision, linkID})
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	// Replace this generator's dependency observations in the same transaction as
	// its rows. Multiple identical relations union their observed inputs.
	if _, err = s.db.Exec(ctx, `DELETE FROM memory_lineage WHERE object_type='relation'
 AND source_kind IN ('memory-relation-input-v1','memory-relation-input-v2') AND object_id IN
 (SELECT object_id FROM memory_lineage WHERE object_type='relation' AND source_kind=$1 AND source_ref=$2)`, derivedIndexSource, fmt.Sprint(id)); err != nil {
		return err
	}
	relationIDs := []int64{}
	inputs := []derivedRelationInput{}
	for _, r := range relations {
		var relationID int64
		if err = s.db.QueryRow(ctx, `WITH existing AS (
 SELECT id FROM memory_relations WHERE memory_id=$1 AND episode_id=$2 AND src_entity=$3
 AND relation=$4 AND dst_entity=$5 AND fact_text=$6 AND valid_at=$7 AND invalid_at=$8 ORDER BY id LIMIT 1
), refreshed AS (UPDATE memory_relations SET weight=$9 WHERE id IN(SELECT id FROM existing) RETURNING id),
 inserted AS (INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text,valid_at,invalid_at,weight)
 SELECT $1,$2,$3,$4,$5,$6,$7,$8,$9 WHERE NOT EXISTS(SELECT 1 FROM existing) RETURNING id)
 SELECT id FROM refreshed UNION ALL SELECT id FROM inserted`, id, parentEpisode, r.Source, r.Relation, r.Target, r.Fact, r.Valid, r.Invalid, r.Weight).Scan(&relationID); err != nil {
			return err
		}
		if err = s.markDerivedIndexObject(ctx, "relation", relationID, id); err != nil {
			return err
		}
		inputs = append(inputs, derivedRelationInput{RelationID: relationID, RecordID: fmt.Sprint(id), RecordRevision: fmt.Sprint(parentRevision)})
		if r.InputID != 0 {
			inputs = append(inputs, derivedRelationInput{RelationID: relationID, RecordID: fmt.Sprint(r.InputID), RecordRevision: fmt.Sprint(r.InputRevision), LinkID: fmt.Sprint(r.InputLinkID)})
		}
		relationIDs = append(relationIDs, relationID)
	}
	if err = s.writeDerivedRelationInputs(ctx, inputs); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `WITH stale AS MATERIALIZED (
 SELECT object_id FROM memory_lineage WHERE object_type='relation' AND source_kind=$2 AND source_ref=$3
 AND NOT(object_id=ANY($4::text::bigint[]))
), removed AS (DELETE FROM memory_relations WHERE memory_id=$1 AND id IN(SELECT object_id FROM stale) RETURNING id)
 DELETE FROM memory_lineage WHERE object_type='relation' AND object_id IN(SELECT id FROM removed)`, id, derivedIndexSource, fmt.Sprint(id), memoryIDsParameter(relationIDs)); err != nil {
		return err
	}
	_, err = s.db.Exec(ctx, `WITH stale AS MATERIALIZED (
 SELECT object_id FROM memory_lineage WHERE object_type='episode' AND source_kind=$2 AND source_ref=$3
 AND NOT(object_id=ANY($4::text::bigint[]))
), removed AS (DELETE FROM memory_episodes e WHERE memory_id=$1 AND id IN(SELECT object_id FROM stale)
 AND NOT EXISTS(SELECT 1 FROM memory_relations r WHERE r.episode_id=e.id) RETURNING id)
 DELETE FROM memory_lineage WHERE object_type='episode' AND object_id IN(SELECT id FROM removed)`, id, derivedIndexSource, fmt.Sprint(id), memoryIDsParameter(episodeIDs))
	return err
}

// The retired C generator owned the parent-key, headline and signals episodes;
// its extracted relations carried their episode ID. Authored/cognified relations
// used episode_id=0. Adopt this old inventory once, before reconciling it, so an
// existing store cannot keep stale C-generated facts after its first Go rebuild.
func (s *postgresDataStore) adoptLegacyDerivedRelations(ctx context.Context, id int64) error {
	_, err := s.db.Exec(ctx, `WITH eligible AS MATERIALIZED (
 SELECT e.id FROM memory_episodes e JOIN memories m ON m.id=e.memory_id WHERE m.id=$1
 AND e.source_session=COALESCE(m.source_session,'') AND e.episode_key IN(m.key,'headline','signals')
 AND NOT EXISTS(SELECT 1 FROM memory_lineage WHERE object_type='memory' AND object_id=$1 AND source_kind=$2 AND source_ref=$3)
), episodes AS (
 INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'episode',id,$2,$3 FROM eligible WHERE NOT EXISTS(SELECT 1 FROM memory_lineage l
 WHERE l.object_type='episode' AND l.object_id=eligible.id AND l.source_kind=$2 AND l.source_ref=$3)
), relations AS (
 INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'relation',r.id,$2,$3 FROM memory_relations r WHERE r.memory_id=$1 AND r.episode_id IN(SELECT id FROM eligible)
 AND NOT EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='relation' AND l.object_id=r.id AND l.source_kind=$2 AND l.source_ref=$3)
) INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',$1,$2,$3 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage
 WHERE object_type='memory' AND object_id=$1 AND source_kind=$2 AND source_ref=$3)`, id, derivedIndexSource, fmt.Sprint(id))
	return err
}

// Capture exact versions from the same row reads that supplied the copied text.
// A later independent SELECT must never certify old text against a newer source.
type derivedRelationInput struct {
	RelationID     int64  `json:"relation_id"`
	RecordID       string `json:"record_id"`
	RecordRevision string `json:"record_revision"`
	LinkID         string `json:"link_id,omitempty"`
}

func (s *postgresDataStore) writeDerivedRelationInputs(ctx context.Context, inputs []derivedRelationInput) error {
	if len(inputs) == 0 {
		return nil
	}
	raw, err := json.Marshal(inputs)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(ctx, `WITH inputs AS (
 SELECT DISTINCT relation_id,jsonb_strip_nulls(jsonb_build_object('record_id',record_id,
 'record_revision',record_revision,'link_id',NULLIF(link_id,'')))::text AS source_ref
 FROM jsonb_to_recordset($1::jsonb) AS x(relation_id bigint,record_id text,record_revision text,link_id text)
 ) INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'relation',i.relation_id,'memory-relation-input-v2',i.source_ref FROM inputs i
 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='relation'
 AND l.object_id=i.relation_id AND l.source_kind='memory-relation-input-v2' AND l.source_ref=i.source_ref)`, string(raw))
	return err
}

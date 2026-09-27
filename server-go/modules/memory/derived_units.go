package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
)

const unitPointOffset int64 = 1000000000000

type derivedUnit struct {
	ID              int64   `json:"-"`
	Type            string  `json:"unit_type"`
	Key             string  `json:"unit_key"`
	Text            string  `json:"unit_text"`
	Kind            string  `json:"memory_kind"`
	Weight          float64 `json:"weight"`
	SummaryID       int64   `json:"input_summary_id"`
	SummaryRevision int64   `json:"input_summary_revision"`
}
type derivedEdge struct {
	Source int64   `json:"source"`
	Target int64   `json:"target"`
	Type   string  `json:"kind"`
	Weight float64 `json:"weight"`
}

func derivedUnitKind(explicit, kind, unitType string, hasEvents bool) string {
	if unitType == "event" || unitType == "temporal" || unitType == "episode" {
		return "episodic"
	}
	if wordIn(explicit, "episodic semantic procedural") && explicit != "" {
		return explicit
	}
	if wordIn(kind, "preference procedure policy workflow") {
		return "procedural"
	}
	if kind == "episode" || hasEvents {
		return "episodic"
	}
	return "semantic"
}

func (s *postgresDataStore) replaceDerivedUnits(ctx context.Context, id int64) error {
	var key, content, kind, explicit string
	var parentRevision int64
	if err := s.db.QueryRow(ctx, `SELECT key,content,kind,cognified_memory_kind,record_revision FROM memories WHERE id=$1`, id).Scan(&key, &content, &kind, &explicit, &parentRevision); err != nil {
		return err
	}
	rows, err := s.db.Query(ctx, `SELECT 'summary',scope,summary,3.0,id,record_revision FROM
 (SELECT summary.scope,summary.summary,summary.id,summary.record_revision FROM memory_summaries summary JOIN memories m ON m.id=summary.memory_id WHERE m.id=$1 AND `+summaryCurrentInputsSQL("summary", "m")+` ORDER BY summary.id LIMIT 8) s
 UNION ALL SELECT 'event',actor||' '||action,concat_ws(' ',actor,action,object,location,event_time),2.8,0::bigint,0::bigint FROM
 (SELECT * FROM memory_event_frames WHERE memory_id=$1 ORDER BY id LIMIT 16) e
 UNION ALL SELECT 'temporal',granularity,ref_key,1.6+weight*0.4,0::bigint,0::bigint FROM
 (SELECT * FROM memory_temporal_refs WHERE memory_id=$1 ORDER BY weight DESC,id LIMIT 16) t
 UNION ALL SELECT 'entity',role,entity,1.4+weight*0.3,0::bigint,0::bigint FROM
 (SELECT * FROM memory_entities WHERE memory_id=$1 ORDER BY weight DESC,id LIMIT 16) n
 UNION ALL SELECT 'chunk','chunk_'||chunk_index,chunk_text,1.2,0::bigint,0::bigint FROM
 (SELECT * FROM memory_chunks WHERE memory_id=$1 ORDER BY chunk_index LIMIT 16) c`, id)
	if err != nil {
		return err
	}
	units := []derivedUnit{}
	hasSummary, hasEvents := false, false
	for rows.Next() {
		var u derivedUnit
		if err = rows.Scan(&u.Type, &u.Key, &u.Text, &u.Weight, &u.SummaryID, &u.SummaryRevision); err != nil {
			rows.Close()
			return err
		}
		hasSummary = hasSummary || u.Type == "summary"
		hasEvents = hasEvents || u.Type == "event" || u.Type == "temporal"
		units = append(units, u)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	if !hasSummary && key != "" {
		units = append(units, derivedUnit{Type: "summary", Key: "fallback", Text: key, Weight: 2})
	}
	if content != "" {
		units = append(units, derivedUnit{Type: "episode", Key: key, Text: content, Weight: 1.8})
	}
	desired := units[:0]
	seen := map[string]bool{}
	for _, u := range units {
		u.Key = textBound(derivedNormalize(u.Key), 255)
		u.Text = textBound(derivedNormalize(u.Text), 511)
		u.Kind = derivedUnitKind(explicit, kind, u.Type, hasEvents)
		identity := u.Type + "\x00" + u.Key + "\x00" + u.Text
		if u.Text != "" && !seen[identity] {
			seen[identity] = true
			desired = append(desired, u)
		}
	}
	encoded, err := json.Marshal(desired)
	if err != nil {
		return err
	}
	rows, err = s.db.Query(ctx, `INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,memory_kind,weight)
 SELECT $1,unit_type,unit_key,unit_text,memory_kind,weight FROM jsonb_to_recordset($2::jsonb)
 AS x(unit_type text,unit_key text,unit_text text,memory_kind text,weight double precision)
 ON CONFLICT(memory_id,unit_type,unit_key,unit_text) DO UPDATE SET
 memory_kind=EXCLUDED.memory_kind,weight=EXCLUDED.weight
 RETURNING id,unit_type`, id, string(encoded))
	if err != nil {
		return err
	}
	ids := []int64{}
	groups := map[string][]int64{}
	for rows.Next() {
		var unitID int64
		var typ string
		if err = rows.Scan(&unitID, &typ); err != nil {
			rows.Close()
			return err
		}
		ids = append(ids, unitID)
		groups[typ] = append(groups[typ], unitID)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `DELETE FROM memory_lineage WHERE object_type='unit'
 AND source_kind='memory-unit-input-v1' AND object_id=ANY($1::text::bigint[])`, memoryIDsParameter(ids)); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'unit',u.id,'memory-index-v1',$2::bigint::text FROM memory_units u WHERE u.id=ANY($1::text::bigint[])
 AND NOT EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='unit' AND l.object_id=u.id AND l.source_kind='memory-index-v1')`, memoryIDsParameter(ids), id); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'unit',u.id,'memory-unit-input-v1',jsonb_build_object('record_id',$1::bigint::text,
 'record_revision',$3::bigint::text,'unit_digest',`+unitInputDigestSQL("u")+`,
 'summary_id',x.input_summary_id::text,'summary_revision',x.input_summary_revision::text)::text
 FROM memory_units u JOIN jsonb_to_recordset($2::jsonb) AS x(unit_type text,unit_key text,unit_text text,input_summary_id bigint,input_summary_revision bigint)
 ON u.unit_type=x.unit_type AND u.unit_key=x.unit_key AND u.unit_text=x.unit_text WHERE u.memory_id=$1`, id, string(encoded), parentRevision); err != nil {
		return err
	}
	// Reuse unchanged unit IDs so episode lineage and external references survive.
	// Only deterministic unit kinds belong to this rebuild; cards and custom units
	// remain owned by their original producer.
	if _, err = s.db.Exec(ctx, `WITH stale AS MATERIALIZED (
 SELECT id FROM memory_units WHERE memory_id=$1 AND is_episode_card=0
 AND unit_type IN ('summary','event','temporal','entity','chunk','episode')
 AND NOT(id=ANY($2::text::bigint[]))
), vectors AS (DELETE FROM memory_embeddings WHERE point_id IN (SELECT $3+id FROM stale)),
 queues AS (DELETE FROM vector_index_ops WHERE point_id IN (SELECT $3+id FROM stale)),
 lineage AS (DELETE FROM memory_lineage WHERE object_type='unit' AND object_id IN (SELECT id FROM stale))
 DELETE FROM memory_units WHERE id IN (SELECT id FROM stale)`, id, memoryIDsParameter(ids), unitPointOffset); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `DELETE FROM memory_unit_edges
 WHERE (src_unit_id=ANY($1::text::bigint[]) AND edge_type IN ('contains','anchored_by','mentions','supports','same_episode','supersedes'))
 OR (dst_unit_id=ANY($1::text::bigint[]) AND edge_type='same_episode')`, memoryIDsParameter(ids)); err != nil {
		return err
	}
	edges := []derivedEdge{}
	connect := func(a, b, typ string, w float64) {
		for _, src := range groups[a] {
			for _, dst := range groups[b] {
				edges = append(edges, derivedEdge{src, dst, typ, w})
			}
		}
	}
	connect("summary", "event", "contains", 1)
	connect("summary", "chunk", "contains", .8)
	connect("event", "temporal", "anchored_by", 1.1)
	connect("event", "entity", "mentions", 1)
	connect("chunk", "event", "supports", .7)
	connect("episode", "summary", "same_episode", 1)
	connect("episode", "event", "same_episode", 1)
	connect("episode", "chunk", "same_episode", .8)
	if len(groups["episode"]) > 0 {
		episode := groups["episode"][0]
		rows, err = s.db.Query(ctx, `SELECT u.id FROM memory_units u JOIN memories m ON m.id=u.memory_id
 JOIN memories source ON source.id=$1 WHERE u.unit_type='episode' AND m.id<>source.id
 AND m.lifecycle_state='active' AND m.source_session=source.source_session AND COALESCE(source.source_session,'')<>''
 AND m.scope_type=source.scope_type AND m.scope_value=source.scope_value ORDER BY u.id LIMIT 64`, id)
		if err != nil {
			return err
		}
		for rows.Next() {
			var other int64
			if err = rows.Scan(&other); err != nil {
				rows.Close()
				return err
			}
			edges = append(edges, derivedEdge{episode, other, "same_episode", .9}, derivedEdge{other, episode, "same_episode", .9})
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return err
		}
		rows, err = s.db.Query(ctx, `SELECT u.id FROM memory_links l JOIN memory_units u ON u.memory_id=l.target_id
 JOIN memories target ON target.id=u.memory_id JOIN memories source ON source.id=l.source_id
 WHERE l.source_id=$1 AND l.relation='supersedes' AND u.unit_type='episode'
 AND target.scope_type=source.scope_type AND target.scope_value=source.scope_value ORDER BY u.id LIMIT 64`, id)
		if err != nil {
			return err
		}
		for rows.Next() {
			var other int64
			if err = rows.Scan(&other); err != nil {
				rows.Close()
				return err
			}
			edges = append(edges, derivedEdge{episode, other, "supersedes", 1.2})
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return err
		}
	}
	encoded, err = json.Marshal(edges)
	if err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_unit_edges(src_unit_id,dst_unit_id,edge_type,weight)
 SELECT DISTINCT source,target,kind,weight FROM jsonb_to_recordset($1::jsonb)
 AS x(source bigint,target bigint,kind text,weight double precision)
 WHERE NOT EXISTS(SELECT 1 FROM memory_unit_edges e WHERE e.src_unit_id=x.source AND e.dst_unit_id=x.target AND e.edge_type=x.kind)`, string(encoded)); err != nil {
		return err
	}
	// Even when the unit text is unchanged, its kind or weight may have changed.
	// Queue a refresh while retaining the previous vector until a replacement is
	// available, using the same failure-preserving policy as parent embeddings.
	_, err = s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,updated_at)
 SELECT $3+u.id,'memory',u.memory_id,'pending',0,'',pg_now_text() FROM memory_units u
 WHERE u.memory_id=$1 AND u.id=ANY($2::text::bigint[])
 ON CONFLICT(point_id) DO UPDATE SET status='pending',attempts=0,last_error='',updated_at=pg_now_text()`, id, memoryIDsParameter(ids), unitPointOffset)
	return err
}

func unitEmbeddingText(u derivedUnit) string {
	switch u.Type {
	case "event":
		return fmt.Sprintf("event actor_action %s details %s weight %.2f", u.Key, u.Text, u.Weight)
	case "temporal":
		return fmt.Sprintf("time reference %s granularity %s weight %.2f", u.Text, u.Key, u.Weight)
	case "entity":
		return fmt.Sprintf("entity role %s value %s weight %.2f", u.Key, u.Text, u.Weight)
	case "summary":
		return fmt.Sprintf("summary scope %s text %s weight %.2f", u.Key, u.Text, u.Weight)
	case "chunk":
		return fmt.Sprintf("evidence chunk %s text %s weight %.2f", u.Key, u.Text, u.Weight)
	default:
		return strings.TrimSpace(fmt.Sprintf("%s %s %s %.2f", u.Type, u.Key, u.Text, u.Weight))
	}
}

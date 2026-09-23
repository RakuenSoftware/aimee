package memory

import (
	"context"
	"errors"
	"strconv"
)

type versionedEpisode struct {
	Episode
	source *typedSourceVersion
}

// Read the episode, canonical parent and owner identity in one SQL snapshot.
// Ordinary episode reads keep their public shape and do not collect versions.
func (s *postgresDataStore) typedEpisodes(ctx context.Context, query string, limit int, exact Scope) ([]versionedEpisode, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT e.id,e.memory_id,e.episode_key,e.episode_text,e.source_session,e.reference_time,e.created_at,
 e.record_revision::text,m.record_revision::text,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 FROM memory_episodes e JOIN memories m ON m.id=e.memory_id
 WHERE ($1='' OR e.episode_key ILIKE '%'||$1||'%' OR e.episode_text ILIKE '%'||$1||'%')
 AND `+currentMemorySQL("m.")+` AND `+currentEpisodeInputsSQL("e")+` AND ($3='' OR (m.scope_type=$3 AND m.scope_value=$4))
 ORDER BY `+domainScopeRankSQL+` DESC,e.reference_time DESC,e.created_at DESC,e.id DESC LIMIT $2`, query, limit, exact.Type, exact.Value)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := []versionedEpisode{}
	for rows.Next() {
		var item versionedEpisode
		source := &typedSourceVersion{Kind: "memory_episode", MemoryParentState: "observed"}
		parent := MemoryRecordVersion{SchemaVersion: 1}
		if err := rows.Scan(&item.ID, &item.MemoryID, &item.Key, &item.Text, &item.SourceSession, &item.ReferenceTime, &item.CreatedAt,
			&source.Version.RecordRevision, &parent.RecordRevision, &source.Version.OwnerID); err != nil {
			return nil, err
		}
		source.Version.SchemaVersion = 1
		source.Version.RecordID = strconv.FormatInt(item.ID, 10)
		parent.OwnerID, parent.RecordID = source.Version.OwnerID, strconv.FormatInt(item.MemoryID, 10)
		if !source.Version.validFor(item.ID) || !parent.validFor(item.MemoryID) {
			return nil, errors.New("memory: episode or parent version unavailable")
		}
		source.MemoryParents = []MemoryRecordVersion{parent}
		item.source = source
		items = append(items, item)
	}
	return items, rows.Err()
}

package memory

import (
	"context"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

// Read an explicitly named retained revision in one snapshot with its private
// owner and surviving parent. Historical payloads never join normal recall.
func (s *postgresDataStore) personalVersion(ctx context.Context, scope Scope, version MemoryRecordVersion) (Record, error) {
	id, _ := strconv.ParseInt(version.RecordID, 10, 64)
	r := Record{Scope: scope, Version: &version, Historical: true}
	err := s.db.QueryRow(ctx, `WITH parent AS MATERIALIZED (
 SELECT m.* FROM user_memories m,user_memory_collection_generation o
 WHERE m.id=$1 AND o.id=1 AND o.owner_id=$2::uuid
   AND m.lifecycle_state IN ('active','retired')
), revisions AS (
 SELECT to_jsonb(p) AS record FROM parent p WHERE record_revision=$3::bigint
 UNION ALL
 SELECT v.record FROM user_memory_versions v JOIN parent p ON p.id=v.memory_id
 WHERE v.record_revision=$3::bigint AND v.record_revision<p.record_revision
)
SELECT $1::bigint,record->>'tier',record->>'kind',record->>'key',record->>'content',
 (record->>'confidence')::double precision FROM revisions
 WHERE record->>'lifecycle_state' IN ('active','retired')`, id, version.OwnerID, version.RecordRevision).
		Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
	if store.IsNoRows(err) {
		return Record{}, ErrMemoryNotFound
	}
	return r, err
}

// One statement locks, compares and corrects. The private history and outbox
// triggers share its transaction, including callers without an explicit Tx.
func (s *postgresDataStore) correctPersonalVersion(ctx context.Context, scope Scope, id int64, content string, confidence float64, expected MemoryRecordVersion) (out Record, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "supersede", ID: id}, DataResponse{Records: []Record{out}}, err, "")
	}()
	content, err = screenMemoryText(content)
	if err != nil {
		return Record{}, err
	}
	out.Scope = scope
	out.Version = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: expected.OwnerID, RecordID: expected.RecordID}
	var found bool
	err = s.db.QueryRow(ctx, `WITH target AS MATERIALIZED (
 SELECT id,record_revision FROM user_memories WHERE id=$1 AND lifecycle_state='active'
   AND (valid_until IS NULL OR valid_until>now()) FOR NO KEY UPDATE
), corrected AS (
 UPDATE user_memories m SET content=$2,confidence=$3,updated_at=now()
 FROM target t,user_memory_collection_generation o
 WHERE m.id=t.id AND o.id=1 AND o.owner_id=$4::uuid AND t.record_revision=$5::bigint
 RETURNING m.id,m.tier,m.kind,m.key,m.content,m.confidence,m.record_revision
)
SELECT EXISTS(SELECT 1 FROM target),COALESCE(c.id,0),COALESCE(c.tier,''),COALESCE(c.kind,''),
 COALESCE(c.key,''),COALESCE(c.content,''),COALESCE(c.confidence,0),COALESCE(c.record_revision::text,'')
 FROM (VALUES(1)) singleton(n) LEFT JOIN corrected c ON true`, id, content, confidence, expected.OwnerID, expected.RecordRevision).
		Scan(&found, &out.ID, &out.Tier, &out.Kind, &out.Key, &out.Content, &out.Confidence, &out.Version.RecordRevision)
	if err != nil {
		return Record{}, err
	}
	if !found {
		return Record{}, ErrMemoryNotFound
	}
	if out.ID == 0 {
		return Record{}, errMutationVersionConflict
	}
	return out, nil
}

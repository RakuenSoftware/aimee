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
	r := Record{Scope: scope, Version: &version, Historical: true, Authorship: &PersonalAuthorship{}}
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
 (record->>'confidence')::double precision,COALESCE(record->>'provenance_category','unknown'),COALESCE(record->>'author_principal',''),COALESCE(record->>'author_transport',''),COALESCE(record->>'reviewer_principal',''),COALESCE(record->>'reviewer_transport',''),COALESCE(record->>'review_proposal_id','') FROM revisions
 WHERE record->>'lifecycle_state' IN ('active','retired')`, id, version.OwnerID, version.RecordRevision).
		Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &r.Authorship.Category, &r.Authorship.Principal, &r.Authorship.Transport, &r.Authorship.Reviewer, &r.Authorship.ReviewTransport, &r.Authorship.ProposalID)
	if store.IsNoRows(err) {
		return Record{}, ErrMemoryNotFound
	}
	return r, err
}

// The common private admission path compares the revision while holding the row
// lock. Retention and change publication remain in the same transaction.
func (s *postgresDataStore) correctPersonalVersion(ctx context.Context, scope Scope, id int64, content string, confidence float64, expected MemoryRecordVersion) (out Record, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "supersede", ID: id}, DataResponse{Records: []Record{out}}, err, "")
	}()
	content, err = screenMemoryText(content)
	if err != nil {
		return Record{}, err
	}
	return s.mutatePersonal(ctx, "supersede", Record{Scope: scope, ID: id, Content: content, Confidence: confidence}, &expected)
}

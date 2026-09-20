package memory

import (
	"context"
	"encoding/json"
	"errors"
)

type MemoryChangeCursor struct {
	OwnerID    string `json:"owner_id"`
	Generation int64  `json:"generation"`
}

type MemoryChangesRequest struct {
	SchemaVersion int                 `json:"schema_version"`
	After         *MemoryChangeCursor `json:"after,omitempty"`
	Limit         int                 `json:"limit,omitempty"`
}

type MemoryInvalidation struct {
	Generation     int64  `json:"generation"`
	MemoryID       int64  `json:"memory_id"`
	RecordRevision int64  `json:"record_revision"`
	Operation      string `json:"operation"`
}

// This feed invalidates derived state; it cannot reconstruct canonical content
// or certify that a consumer has applied a change. Initial consumers and any
// owner/retention gap require an owner-bound canonical snapshot before reuse.
type MemoryChangePage struct {
	SchemaVersion    int                  `json:"schema_version"`
	Head             MemoryChangeCursor   `json:"head"`
	Next             MemoryChangeCursor   `json:"next"`
	SnapshotRequired bool                 `json:"snapshot_required"`
	More             bool                 `json:"more"`
	Events           []MemoryInvalidation `json:"events"`
}

func (r MemoryChangesRequest) valid() bool {
	return r.SchemaVersion == 1 && r.Limit >= 0 && r.Limit <= 256 &&
		(r.After == nil || (r.After.OwnerID != "" && len(r.After.OwnerID) <= 64 && r.After.Generation >= 0))
}

func (s *postgresDataStore) personalChanges(ctx context.Context, request MemoryChangesRequest) (MemoryChangePage, error) {
	if s.placement != PlacementServer || !request.valid() {
		return MemoryChangePage{}, errors.New("memory: unsupported change feed request")
	}
	limit := request.Limit
	if limit == 0 {
		limit = 64
	}
	after := int64(0)
	owner := ""
	if request.After != nil {
		after, owner = request.After.Generation, request.After.OwnerID
	}
	// Head and page use one storage statement snapshot. Never read the head,
	// wait for another transaction to commit, and claim its rows share that head.
	var raw string
	err := s.db.QueryRow(ctx, `WITH head AS MATERIALIZED (
 SELECT owner_id::text AS owner_id,generation FROM user_memory_collection_generation WHERE id=1
), events AS (
 SELECT e.generation,e.memory_id,e.record_revision,e.operation
 FROM user_memory_invalidation_outbox e,head h
 WHERE $1=h.owner_id AND $2<=h.generation AND e.generation>$2 AND e.generation<=h.generation
 ORDER BY e.generation LIMIT $3
)
SELECT jsonb_build_object('head',to_jsonb(h),'events',
 COALESCE((SELECT jsonb_agg(to_jsonb(e) ORDER BY e.generation) FROM events e),'[]'::jsonb))::text FROM head h`,
		owner, after, limit).Scan(&raw)
	if err != nil {
		return MemoryChangePage{}, err
	}
	var page MemoryChangePage
	if err := json.Unmarshal([]byte(raw), &page); err != nil {
		return MemoryChangePage{}, err
	}
	page.SchemaVersion = 1
	page.Next = MemoryChangeCursor{OwnerID: page.Head.OwnerID, Generation: after}
	page.SnapshotRequired = request.After == nil || owner != page.Head.OwnerID || after > page.Head.Generation
	if !page.SnapshotRequired {
		for _, event := range page.Events {
			if event.Generation != page.Next.Generation+1 {
				page.SnapshotRequired = true
				break
			}
			page.Next.Generation = event.Generation
		}
		if len(page.Events) < limit && page.Next.Generation != page.Head.Generation {
			page.SnapshotRequired = true
		}
	}
	if page.SnapshotRequired {
		page.Events = []MemoryInvalidation{}
		page.Next = page.Head // snapshot boundary, never a replay acknowledgement
	} else {
		page.More = page.Next.Generation < page.Head.Generation
	}
	return page, nil
}

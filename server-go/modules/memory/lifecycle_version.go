package memory

import (
	"context"
	"errors"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

// Keep the row locked through rejection/restoration and its audit/invalidation
// writes. RLS admits the target before comparing versions: knowledge of an old
// version must not disclose a record outside the caller's current scope.
func (s *postgresDataStore) lockKBLifecycleVersion(ctx context.Context, id int64, expected *MemoryRecordVersion) error {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !expected.validFor(id) {
		return errors.New("memory: invalid conditional lifecycle mutation")
	}
	observed := MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}
	err := s.db.QueryRow(ctx, `SELECT record_revision::text,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 FROM memories WHERE id=$1 FOR UPDATE`, id).Scan(&observed.RecordRevision, &observed.OwnerID)
	if store.IsNoRows(err) {
		return ErrMemoryNotFound
	}
	if err != nil {
		return err
	}
	if observed != *expected {
		return errMutationVersionConflict
	}
	return nil
}

package memory

import (
	"context"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

// The Go worker drives a private durable projection consumer. Each storage
// application queues at most 64 roots; 16 pages bound work per scheduler tick.
// Checkpoints describe applied invalidation, never completed derivation or release
// authority. Serving continues to compare canonical source versions directly.
func (s *postgresDataStore) reconcileRelationInputs(ctx context.Context) error {
	db, ok := s.db.(store.DB)
	if s.placement != PlacementKB || !ok {
		return errors.New("memory: relation invalidation requires shared transactions")
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(context.Background())
	if err = sharedIndexContext(ctx, tx); err != nil {
		return err
	}
	for page := 0; page < 16; page++ {
		if _, err = tx.Exec(ctx, `SELECT memory_apply_relation_invalidations(64)`); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

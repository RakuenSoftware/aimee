package db1

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strings"
)

// FrozenCreate is one path first introduced by a slice's immutable diff. The
// content hash is the Git blob identity, so text and binary files share the
// same exact equality rule.
type FrozenCreate struct {
	Path        string
	ContentHash string
}

// FrozenCreateConflict identifies two sibling slices that froze different
// content for the same newly-created path.
type FrozenCreateConflict struct {
	Path                string
	ExistingWorkItem    string
	ConflictingWorkItem string
}

// ClaimFrozenCreates atomically publishes every new path in a slice's frozen
// diff. Identical sibling creations coexist. A divergent claim rolls back as a
// unit, so simultaneous freezes have exactly one winner and never leave a
// partial path set behind.
func (s *Store) ClaimFrozenCreates(ctx context.Context, parentID, workItemID string,
	creates []FrozenCreate) (*FrozenCreateConflict, error) {
	if strings.TrimSpace(parentID) == "" || strings.TrimSpace(workItemID) == "" {
		return nil, errors.New("frozen-create parent and work item are required")
	}
	if len(creates) == 0 {
		return nil, nil
	}
	ordered := append([]FrozenCreate(nil), creates...)
	sort.Slice(ordered, func(i, j int) bool { return ordered[i].Path < ordered[j].Path })
	normalized := make([]FrozenCreate, 0, len(ordered))
	for _, create := range ordered {
		if create.Path == "" || create.ContentHash == "" {
			return nil, errors.New("frozen-create path and content hash are required")
		}
		if len(normalized) > 0 && normalized[len(normalized)-1].Path == create.Path {
			if normalized[len(normalized)-1].ContentHash != create.ContentHash {
				return nil, fmt.Errorf("frozen diff contains divergent duplicate path %q", create.Path)
			}
			continue
		}
		normalized = append(normalized, create)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("begin frozen-create claim: %w", err)
	}
	defer tx.Rollback()
	// Make the first statement a write so correctness does not depend on the
	// connection's transaction-lock mode. This reserves SQLite's sole writer
	// before the conflict read: two connections can never both observe an empty
	// claim set and then race independent inserts whose primary keys differ.
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET updated_at=updated_at
WHERE work_item_id=? AND parent_id=? AND state='active'`, workItemID, parentID)
	if err != nil {
		return nil, fmt.Errorf("validate frozen-create owner: %w", err)
	}
	eligible, err := result.RowsAffected()
	if err != nil {
		return nil, fmt.Errorf("count frozen-create owner: %w", err)
	}
	if eligible != 1 {
		return nil, errors.New("frozen-create owner is not an active child of the parent")
	}
	for _, create := range normalized {
		var existing string
		err := tx.QueryRowContext(ctx, `SELECT work_item_id FROM wfe_frozen_create
WHERE parent_id=? AND path=? AND work_item_id<>? AND content_hash<>?
ORDER BY work_item_id LIMIT 1`, parentID, create.Path, workItemID, create.ContentHash).Scan(&existing)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return nil, fmt.Errorf("check frozen-create claim: %w", err)
		}
		if err == nil {
			return &FrozenCreateConflict{Path: create.Path, ExistingWorkItem: existing,
				ConflictingWorkItem: workItemID}, nil
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO wfe_frozen_create
(parent_id,path,work_item_id,content_hash) VALUES (?,?,?,?)
ON CONFLICT(parent_id,path,work_item_id) DO UPDATE SET
content_hash=excluded.content_hash,updated_at=datetime('now')`,
			parentID, create.Path, workItemID, create.ContentHash); err != nil {
			return nil, fmt.Errorf("publish frozen-create claim: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("commit frozen-create claim: %w", err)
	}
	return nil, nil
}

package workflowstore

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"

	wire "github.com/JBailes/aimee/server-go/aimee"
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
//
// The normalisation below stays on this side and the transaction went to the
// module, which is the split the port used everywhere: deciding what to claim
// is the engine's business, and claiming it atomically is the store's.
func (s *Store) ClaimFrozenCreates(ctx context.Context, parentID, workItemID string,
	creates []FrozenCreate) (*FrozenCreateConflict, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	if strings.TrimSpace(parentID) == "" || strings.TrimSpace(workItemID) == "" {
		return nil, errors.New("frozen-create parent and work item are required")
	}
	if len(creates) == 0 {
		return nil, nil
	}
	// Sorted and de-duplicated before it crosses: two entries for one path that
	// disagree are a malformed diff, not a conflict between slices, and saying
	// so here names the actual problem.
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

	items := make([]wire.WfeClaimFrozenCreatesItem, 0, len(normalized))
	for _, create := range normalized {
		items = append(items, wire.WfeClaimFrozenCreatesItem{
			Path:        create.Path,
			ContentHash: create.ContentHash,
		})
	}
	got, err := s.client.WfeClaimFrozenCreates(ctx, parentID, workItemID, len(items), items)
	if err != nil {
		return nil, fmt.Errorf("claim frozen creates: %w", err)
	}
	// An empty path is the module's way of saying there was no conflict. The
	// alternative -- a separate boolean -- would give two ways to say the same
	// thing, and eventually they disagree.
	if got.Path == "" {
		return nil, nil
	}
	return &FrozenCreateConflict{
		Path:                got.Path,
		ExistingWorkItem:    got.ExistingWorkItem,
		ConflictingWorkItem: got.ConflictingWorkItem,
	}, nil
}

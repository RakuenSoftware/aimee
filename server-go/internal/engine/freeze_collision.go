package engine

import (
	"context"
	"fmt"
	"strings"
	"sync"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

const freezeCreateCreateCollision = "freeze_create_create_collision"

type freezeCreate struct {
	Path string
	Blob string
}

type freezeCollisionLockSet struct {
	mu    sync.Mutex
	locks map[string]*sync.Mutex
}

func (s *freezeCollisionLockSet) lock(parentID string) func() {
	if parentID == "" {
		return func() {}
	}
	s.mu.Lock()
	if s.locks == nil {
		s.locks = make(map[string]*sync.Mutex)
	}
	lock := s.locks[parentID]
	if lock == nil {
		lock = &sync.Mutex{}
		s.locks[parentID] = lock
	}
	s.mu.Unlock()
	lock.Lock()
	return lock.Unlock
}

func freezeCreatedFiles(ctx context.Context, workdir, base, head string) (map[string]freezeCreate, error) {
	out, err := gitText(ctx, workdir, "diff", "--name-status", "--diff-filter=A", "-z", base+"..."+head)
	if err != nil {
		return nil, err
	}
	creates := map[string]freezeCreate{}
	fields := strings.Split(out, "\x00")
	for i := 0; i+1 < len(fields); i += 2 {
		if fields[i] == "" || fields[i+1] == "" {
			continue
		}
		path := fields[i+1]
		baseBlob, err := gitText(ctx, workdir, "rev-parse", "--verify", "--quiet", base+":"+path)
		if err == nil && strings.TrimSpace(baseBlob) != "" {
			continue
		}
		blob, err := gitText(ctx, workdir, "rev-parse", head+":"+path)
		if err != nil {
			return nil, fmt.Errorf("resolve created blob %s: %w", path, err)
		}
		creates[path] = freezeCreate{Path: path, Blob: strings.TrimSpace(blob)}
	}
	return creates, nil
}

func frozenSiblingCreatedFiles(ctx context.Context, item db1.WorkItem, workdir, base, head string) (map[string]freezeCreate, error) {
	head = strings.TrimSpace(head)
	if head == "" {
		return nil, nil
	}
	mergeBase, err := gitText(ctx, workdir, "merge-base", base, head)
	if err != nil || strings.TrimSpace(mergeBase) != strings.TrimSpace(base) {
		return nil, nil
	}
	return freezeCreatedFiles(ctx, workdir, base, head)
}

func (r *NativeRunner) rejectDivergentSiblingCreates(ctx context.Context, item db1.WorkItem, workdir, base, head string) error {
	if item.ParentID == "" {
		return nil
	}
	if r.db == nil || r.artifacts == nil {
		return nil
	}
	creates, err := freezeCreatedFiles(ctx, workdir, base, head)
	if err != nil {
		return err
	}
	if len(creates) == 0 {
		return nil
	}
	siblings, err := r.db.Children(ctx, item.ParentID)
	if err != nil {
		return err
	}
	for _, sibling := range siblings {
		if sibling.ID == item.ID || sibling.State != "active" {
			continue
		}
		artifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze")
		if err != nil || artifact.Type != "frozen_diff" || strings.TrimSpace(string(artifact.Content)) == "" {
			continue
		}
		siblingHead := sibling.ContentHash
		if strings.TrimSpace(siblingHead) == "" {
			headArtifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze-head")
			if err == nil && headArtifact.Type == "commit" {
				siblingHead = string(headArtifact.Content)
			}
		}
		siblingCreates, err := frozenSiblingCreatedFiles(ctx, sibling, workdir, base, siblingHead)
		if err != nil {
			return err
		}
		for path, create := range creates {
			if other, ok := siblingCreates[path]; ok && other.Blob != create.Blob {
				return fmt.Errorf("%s: path %s current slice %s conflicts with already frozen sibling slice %s", freezeCreateCreateCollision, path, item.ID, sibling.ID)
			}
		}
	}
	return nil
}

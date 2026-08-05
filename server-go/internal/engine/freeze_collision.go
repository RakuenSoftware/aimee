package engine

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

const freezeCreateCreateCollision = "freeze_create_create_collision"

type freezeCreate struct {
	Path string
	Blob string
}

type freezeCollisionLockSet struct{}

func (s *freezeCollisionLockSet) lock(ctx context.Context, parentID string) (func(), error) {
	if parentID == "" {
		return func() {}, nil
	}
	// The server normally has one process, but appliance upgrades and operator
	// restarts can overlap briefly. A host-local flock preserves compare-and-
	// record atomicity across that boundary; process death releases it.
	key := sha256.Sum256([]byte(parentID))
	path := filepath.Join(os.TempDir(), fmt.Sprintf("aimee-wfe-freeze-%x.lock", key))
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open freeze collision lock: %w", err)
	}
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		err = syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
		if err == nil {
			break
		}
		if !errors.Is(err, syscall.EWOULDBLOCK) && !errors.Is(err, syscall.EAGAIN) {
			_ = file.Close()
			return nil, fmt.Errorf("acquire freeze collision lock: %w", err)
		}
		select {
		case <-ctx.Done():
			_ = file.Close()
			return nil, ctx.Err()
		case <-ticker.C:
		}
	}
	return func() {
		_ = syscall.Flock(int(file.Fd()), syscall.LOCK_UN)
		_ = file.Close()
	}, nil
}

// lockStep is consumed by Engine.Advance. The engine deliberately owns the
// lock lifetime so a sibling cannot run its comparison between this runner's
// artifact publication and the durable Move out of freeze.
func (r *NativeRunner) lockStep(ctx context.Context, req StepRequest) (func(), error) {
	if req.Node.Block != "freeze" {
		return func() {}, nil
	}
	return r.freezeLocks.lock(ctx, req.WorkItem.ParentID)
}

func freezeCreatedFiles(ctx context.Context, workdir, base, head string) (map[string]freezeCreate, error) {
	out, err := gitText(ctx, workdir, "diff", "--name-status", "--diff-filter=A", "-z", base+"..."+head)
	if err != nil {
		return nil, err
	}
	creates := map[string]freezeCreate{}
	// --diff-filter=A limits --name-status -z output to status/path pairs.
	fields := strings.Split(out, "\x00")
	for i := 0; i+1 < len(fields); i += 2 {
		if fields[i] == "" || fields[i+1] == "" {
			continue
		}
		if fields[i] != "A" {
			return nil, fmt.Errorf("unexpected created-file status %q for %s", fields[i], fields[i+1])
		}
		path := fields[i+1]
		blob, err := gitText(ctx, workdir, "rev-parse", head+":"+path)
		if err != nil {
			return nil, fmt.Errorf("resolve created blob %s: %w", path, err)
		}
		creates[path] = freezeCreate{Path: path, Blob: strings.TrimSpace(blob)}
	}
	return creates, nil
}

func frozenSiblingCreatedFiles(ctx context.Context, workdir, base, siblingBase, head string) (map[string]freezeCreate, error) {
	head = strings.TrimSpace(head)
	siblingBase = strings.TrimSpace(siblingBase)
	if head == "" || siblingBase == "" {
		return nil, errors.New("frozen sibling is missing its base or head commit")
	}
	if siblingBase != strings.TrimSpace(base) {
		return nil, nil
	}
	mergeBase, err := gitText(ctx, workdir, "merge-base", base, head)
	if err != nil {
		return nil, fmt.Errorf("resolve frozen sibling merge base: %w", err)
	}
	if strings.TrimSpace(mergeBase) != strings.TrimSpace(base) {
		return nil, fmt.Errorf("frozen sibling head %s is not based on recorded base %s", head, base)
	}
	return freezeCreatedFiles(ctx, workdir, base, head)
}

// siblingStageHasFrozenDiff distinguishes a durable freeze from both an orphan
// marker (process death after writing the artifact but before Move) and a stale
// marker after a review/CI loop returned the sibling to implementation.
func (r *NativeRunner) siblingStageHasFrozenDiff(sibling db1.WorkItem) (bool, error) {
	if sibling.Stage == "freeze" {
		return false, nil
	}
	if r.workflows == nil {
		// Unit-level callers that do not install a registry still exercise the
		// durable-stage rule: any stage other than freeze represents a completed
		// Move in those fixtures.
		return true, nil
	}
	def, err := r.workflows.LoadVersion(sibling.WorkflowName, sibling.WorkflowVersion)
	if err != nil {
		return false, fmt.Errorf("load sibling workflow: %w", err)
	}
	start, ok := def.Node("freeze")
	if !ok {
		return false, errors.New("sibling workflow has no freeze node")
	}
	queue := []string{start.Next, start.OnPass}
	seen := map[string]bool{"freeze": true}
	for len(queue) > 0 {
		id := queue[0]
		queue = queue[1:]
		if id == "" || seen[id] {
			continue
		}
		seen[id] = true
		node, ok := def.Node(id)
		if !ok {
			return false, fmt.Errorf("sibling workflow stage %q is unavailable", id)
		}
		// Crossing a mutation node invalidates the previous frozen snapshot. A
		// later successful freeze starts a new traversal and replaces artifacts.
		if node.Block == "implement" || node.Block == "document" || node.Block == "freeze" {
			continue
		}
		if id == sibling.Stage {
			return true, nil
		}
		queue = append(queue, node.Next, node.OnPass, node.OnFail)
	}
	return false, nil
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
		frozen, err := r.siblingStageHasFrozenDiff(sibling)
		if err != nil {
			return err
		}
		if !frozen {
			continue
		}
		artifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze")
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return fmt.Errorf("read sibling freeze artifact: %w", err)
		}
		if artifact.Type != "frozen_diff" || strings.TrimSpace(string(artifact.Content)) == "" {
			return errors.New("sibling freeze artifact has invalid type or empty content")
		}
		headArtifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze-head")
		if err != nil {
			return fmt.Errorf("read sibling freeze head: %w", err)
		}
		if headArtifact.Type != "commit" {
			return errors.New("sibling freeze head has invalid type")
		}
		baseArtifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze-base")
		if err != nil {
			return fmt.Errorf("read sibling freeze base: %w", err)
		}
		if baseArtifact.Type != "commit" {
			return errors.New("sibling freeze base has invalid type")
		}
		siblingCreates, err := frozenSiblingCreatedFiles(ctx, workdir, base,
			string(baseArtifact.Content), string(headArtifact.Content))
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

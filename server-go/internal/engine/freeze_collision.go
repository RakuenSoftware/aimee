package engine

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

const freezeCreateCreateCollision = "freeze_create_create_collision"

type freezeCreate struct {
	Path string
	Blob string
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

// frozenSiblingCreatedFiles derives the frozen sibling's created files from the
// sibling's durable freeze-base and freeze-head artifacts (already loaded by
// the type-validation path) rather than from the sibling's on-disk Worktree.
// Anchoring the computation to those artifacts keeps the comparison correct
// even when the Worktree directory is empty, missing, or detached from the
// feature branch the sibling was frozen against.
//
// rejectDivergentSiblingCreates is responsible for validating the
// freeze-base/freeze-head artifact type and content (commit type, non-empty
// payload) before they reach this helper, so the only guard here is the
// empty-string trim.
func frozenSiblingCreatedFiles(ctx context.Context, workdir, siblingBase, head string) (map[string]freezeCreate, error) {
	head = strings.TrimSpace(head)
	siblingBase = strings.TrimSpace(siblingBase)
	if head == "" || siblingBase == "" {
		return nil, errors.New("frozen sibling is missing its base or head commit")
	}
	return freezeCreatedFiles(ctx, workdir, siblingBase, head)
}

// resolveSiblingCompareDir picks a directory in which the sibling's
// freeze-base and freeze-head commits are resolvable for the diff. When
// sibling freezes land on a distinct worktree, those commits live behind the
// sibling's per-worktree branch ref and are not reachable from the current
// workdir by construction, so naively diffing against base...head in workdir
// conflates "genuine cannot compare" with "genuine identical create" and
// surfaces a false freeze_create_create_collision.
//
// We resolve the sibling's commits in two steps:
//  1. Prefer the sibling's recorded Worktree: when it is set and points at a
//     directory in which both commits resolve, the sibling's own branch ref
//     makes the comparison decidable without any network or fetch.
//  2. Otherwise try fetching the sibling's branch into the current workdir's
//     shared bare mirror (origin) so the commits become reachable there. This
//     covers the case where the sibling's on-disk Worktree has been pruned
//     but the remote / mirror still holds the branch.
//
// When neither step exposes the commits we return an error so the caller can
// surface the residual freezeSiblingUncomparableError (the genuine "cannot
// compare" case) instead of a false collision rejection.
func resolveSiblingCompareDir(ctx context.Context, sibling db1.WorkItem, workdir, siblingBase, siblingHead string) (string, error) {
	siblingBase = strings.TrimSpace(siblingBase)
	siblingHead = strings.TrimSpace(siblingHead)
	if siblingBase == "" || siblingHead == "" {
		return "", errors.New("frozen sibling is missing its base or head commit")
	}
	candidateSibling := strings.TrimSpace(sibling.Worktree)
	candidateCurrent := strings.TrimSpace(workdir)
	if candidateCurrent != "" && candidateCurrent != candidateSibling {
		// Bring the sibling branch into the current workdir via the shared
		// bare mirror so its commits become reachable there. Errors here are
		// non-fatal: we fall through to the per-candidate probe below.
		branch := "aimee/wi/" + sibling.ID
		_, _ = gitText(ctx, candidateCurrent, "fetch", "--quiet", "origin",
			"+refs/heads/"+branch+":refs/remotes/origin/"+branch)
	}
	for _, dir := range []string{candidateSibling, candidateCurrent} {
		if dir == "" {
			continue
		}
		if _, err := gitText(ctx, dir, "diff", "--name-only", siblingBase+"..."+siblingHead); err == nil {
			return dir, nil
		}
	}
	return "", errors.New("sibling freeze commits are not reachable from the sibling's worktree or the current workdir")
}

// siblingStageHasFrozenDiff distinguishes a durable freeze from both an orphan
// marker (process death after writing the artifact but before Move) and a stale
// marker after a review/CI loop returned the sibling to implementation.
func (r *NativeRunner) siblingStageHasFrozenDiff(sibling db1.WorkItem) (bool, error) {
	if sibling.Stage == "freeze" {
		return false, nil
	}
	if r.workflows == nil {
		return false, errors.New("workflow registry is required for sibling freeze collision inspection")
	}
	def, err := r.workflows.LoadVersion(sibling.WorkflowName, sibling.WorkflowVersion)
	if err != nil {
		return false, fmt.Errorf("load sibling workflow: %w", err)
	}
	start, ok := def.Node("freeze")
	if !ok {
		return false, errors.New("sibling workflow has no freeze node")
	}
	queue := []string{start.Next, start.OnPass, start.OnFail}
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

// freezeCreateCreateCollisionError reports a genuine create/create collision
// on path between currentSlice and siblingSlice.
func freezeCreateCreateCollisionError(path, currentSlice, siblingSlice string) error {
	return fmt.Errorf("%s: path %s current slice %s conflicts with already frozen sibling slice %s", freezeCreateCreateCollision, path, currentSlice, siblingSlice)
}

// freezeUncomparablePathIdentifier picks a stable path-equivalent identifier
// from the current slice's create set so fail-closed errors can name a path
// even though no overlap has been enumerated. The map iteration order is
// non-deterministic, so the identifier is picked deterministically (smallest
// path) rather than at random.
func freezeUncomparablePathIdentifier(creates map[string]freezeCreate) string {
	var chosen string
	for path := range creates {
		if chosen == "" || path < chosen {
			chosen = path
		}
	}
	return chosen
}

// freezeSiblingUncomparableError reports that the sibling's durable freeze
// artifacts (or the comparison itself) are unavailable, so the collision check
// cannot establish whether the slices overlap. It uses the same
// freeze_create_create_collision prefix as the genuine-collision error so a
// caller matching on the prefix catches both. The path argument is a
// path-equivalent identifier drawn from the current slice's create set
// (typically the first created path) so the error names the path the current
// slice was creating when comparison became impossible; this satisfies the
// fail-closed error contract that requires the conflicting path to appear
// alongside both slice names even though no overlap has been enumerated yet.
// Genuine collisions surface the path via freezeCreateCreateCollisionError;
// this error is the distinct "cannot compare" shape that keeps operator
// diagnostics honest. The current slice fails closed rather than silently
// allowing a potentially colliding freeze.
func freezeSiblingUncomparableError(currentSlice, siblingSlice, path string, cause error) error {
	msg := fmt.Sprintf("%s: cannot compare path %s: already frozen sibling slice %s while processing current slice %s", freezeCreateCreateCollision, path, siblingSlice, currentSlice)
	if cause == nil {
		return errors.New(msg)
	}
	return fmt.Errorf("%s: %w", msg, cause)
}

func (r *NativeRunner) rejectDivergentSiblingCreates(ctx context.Context, item db1.WorkItem, workdir, base, head string) error {
	if item.ParentID == "" {
		return nil
	}
	if r.db == nil || r.artifacts == nil {
		return nil
	}
	if r.workflows == nil {
		// A workflow-less runner cannot determine which sibling stages are
		// frozen vs. transient, so sibling collision detection cannot run.
		// Fail closed: a colliding create must not be silently allowed.
		return errors.New("workflow registry is required for sibling freeze collision inspection")
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
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), fmt.Errorf("read sibling freeze artifact: %w", err))
		}
		if artifact.Type != "frozen_diff" || strings.TrimSpace(string(artifact.Content)) == "" {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		headArtifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze-head")
		if errors.Is(err, os.ErrNotExist) {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), fmt.Errorf("read sibling freeze head: %w", err))
		}
		if headArtifact.Type != "commit" {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		baseArtifact, err := r.artifacts.NodeArtifact(sibling.ID, "freeze-base")
		if errors.Is(err, os.ErrNotExist) {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), fmt.Errorf("read sibling freeze base: %w", err))
		}
		if baseArtifact.Type != "commit" {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), nil)
		}
		siblingDir, err := resolveSiblingCompareDir(ctx, sibling, workdir,
			string(baseArtifact.Content), string(headArtifact.Content))
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), err)
		}
		siblingCreates, err := frozenSiblingCreatedFiles(ctx, siblingDir,
			string(baseArtifact.Content), string(headArtifact.Content))
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), err)
		}
		for path, create := range creates {
			if other, ok := siblingCreates[path]; ok && other.Blob != create.Blob {
				return freezeCreateCreateCollisionError(path, item.ID, sibling.ID)
			}
		}
	}
	return nil
}
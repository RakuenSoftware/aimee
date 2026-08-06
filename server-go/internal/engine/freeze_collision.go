package engine

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

const freezeCreateCreateCollision = "freeze_create_create_collision"

// ErrFreezeCreateCreateCollision is the typed sentinel for a freeze-vs-create
// collision between sibling slices. Callers can detect it via errors.Is(err,
// ErrFreezeCreateCreateCollision) instead of string-prefix matching. The
// human-readable path/slice detail remains accessible via err.Error().
var ErrFreezeCreateCreateCollision = errors.New(freezeCreateCreateCollision)

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

// freezeCreateCreateCollisionError reports a genuine create/create collision
// on path between currentSlice and siblingSlice.
func freezeCreateCreateCollisionError(path, currentSlice, siblingSlice string) error {
	return fmt.Errorf("%w: path %s current slice %s conflicts with already frozen sibling slice %s", ErrFreezeCreateCreateCollision, path, currentSlice, siblingSlice)
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
// cannot establish whether the slices overlap. It wraps
// ErrFreezeCreateCreateCollision so callers matching the typed sentinel via
// errors.Is catch both genuine and un-comparable shapes, and its message
// starts with the same freeze_create_create_collision prefix as the
// genuine-collision error so the operator-facing detail reads the same way.
// The path argument is a
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
	if cause == nil {
		return fmt.Errorf("%w: cannot compare path %s: already frozen sibling slice %s while processing current slice %s", ErrFreezeCreateCreateCollision, path, siblingSlice, currentSlice)
	}
	return fmt.Errorf("%w: cannot compare path %s: already frozen sibling slice %s while processing current slice %s: %w", ErrFreezeCreateCreateCollision, path, siblingSlice, currentSlice, cause)
}

// frozenSiblingArtifacts captures the three durable NodeArtifacts that
// together mark a sibling as having a durable frozen diff: the freeze node's
// frozen_diff content and the freeze-head/freeze-base commit SHAs. Anchoring
// the collision check to this triple (rather than the sibling's current
// workflow stage) keeps the answer independent of any post-freeze routing --
// including a review/CI loop that returned the sibling to implement for
// revisions, where the current stage is implement but the frozen diff is still
// on disk.
//
// frozenSiblingArtifacts distinguishes three failure modes:
//   - A genuine read error from the artifact store (decoding failure, I/O
//     error, integrity mismatch) surfaces as a non-nil err so the caller can
//     fail closed with freeze_create_create_collision -- the runner cannot
//     tell whether a sibling's freeze is competing, so it must not silently
//     allow it through on a transient store error. Integrity-validation
//     failures (NodeArtifact wrapping "node artifact failed integrity
//     validation") are explicitly propagated here, not silently skipped.
//   - A missing artifact (os.ErrNotExist / fs.ErrNotExist, also surfaced
//     structurally as wfe.ErrArtifactNotExist) returns ok=false with a nil err.
//     The sibling simply is not frozen (per the durable-triple definition),
//     so the caller skips it as the previous siblingStageHasFrozenDiff
//     continue path did.
//   - A wrong-typed artifact (type does not match the expected frozen_diff
//     or commit) also returns ok=false with a nil err. Wrong-typed artifacts
//     are treated the same as missing ones: the durable triple is not
//     present, so the sibling is not frozen and the caller skips it. This
//     matches the acceptance criterion that only the durable triple marks
//     a sibling as frozen, and prevents the collision check from spuriously
//     rejecting slices whose active sibling carries a partial freeze-artifact
//     set.
func frozenSiblingArtifacts(siblingID string, store *wfe.ArtifactStore) (head, base string, ok bool, err error) {
	if store == nil {
		return "", "", false, nil
	}
	diff, err := store.NodeArtifact(siblingID, "freeze")
	if err != nil {
		if errors.Is(err, os.ErrNotExist) || errors.Is(err, wfe.ErrArtifactNotExist) {
			return "", "", false, nil
		}
		return "", "", false, err
	}
	if diff.Type != "frozen_diff" || strings.TrimSpace(string(diff.Content)) == "" {
		return "", "", false, nil
	}
	headArt, err := store.NodeArtifact(siblingID, "freeze-head")
	if err != nil {
		if errors.Is(err, os.ErrNotExist) || errors.Is(err, wfe.ErrArtifactNotExist) {
			return "", "", false, nil
		}
		return "", "", false, err
	}
	if headArt.Type != "commit" {
		return "", "", false, nil
	}
	baseArt, err := store.NodeArtifact(siblingID, "freeze-base")
	if err != nil {
		if errors.Is(err, os.ErrNotExist) || errors.Is(err, wfe.ErrArtifactNotExist) {
			return "", "", false, nil
		}
		return "", "", false, err
	}
	if baseArt.Type != "commit" {
		return "", "", false, nil
	}
	return string(headArt.Content), string(baseArt.Content), true, nil
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
		siblingHead, siblingBase, frozen, readErr := frozenSiblingArtifacts(sibling.ID, r.artifacts)
		if readErr != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), readErr)
		}
		if !frozen {
			continue
		}
		siblingDir, err := resolveSiblingCompareDir(ctx, sibling, workdir, siblingBase, siblingHead)
		if err != nil {
			return freezeSiblingUncomparableError(item.ID, sibling.ID, freezeUncomparablePathIdentifier(creates), err)
		}
		siblingCreates, err := frozenSiblingCreatedFiles(ctx, siblingDir, siblingBase, siblingHead)
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
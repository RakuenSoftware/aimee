package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMinhashDeleteFile,
		db2contract.OperationMinhashDeleteFile, minhashDeleteFile)
	Register(db2contract.StageUniqueFileBasename,
		db2contract.OperationUniqueFileBasename, uniqueFileBasename)
	Register(db2contract.StageEntityEdgeBumpUtility,
		db2contract.OperationEntityEdgeBumpUtility, entityEdgeBumpUtility)
	Register(db2contract.StageArtifactFlagReview,
		db2contract.OperationArtifactFlagReview, artifactFlagReview)
	Register(db2contract.StageVerdictSuppressed,
		db2contract.OperationVerdictSuppressed, verdictSuppressed)
}

// Both scoped to the current generation by subquery rather than by argument,
// like their whole-project neighbours: a caller cannot ask them to clear a
// published generation, which is what stops re-indexing one file from punching
// a hole in the index somebody is reading.
const (
	minhashDeleteFileSignatureQuery = `DELETE FROM kb_minhash_signatures
 WHERE project = $1 AND file_path = $2
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current')`
	minhashDeleteFileBucketsQuery = `DELETE FROM kb_lsh_buckets
 WHERE project = $1 AND file_path = $2
 AND generation = (SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current')`
)

// minhashDeleteFile forgets one file's similarity signature and the LSH buckets
// built over it.
//
// Two statements in one transaction, which the C does not do -- it deletes the
// signature, then calls lsh_bucket_delete_file, and a failure between them
// leaves buckets naming a signature that is gone. That is the same defect the
// whole-project delete carried, at one file's granularity: a bucket pointing at
// nothing is not a partly-cleared index, it is a wrong one, and the next
// similarity read uses it.
func minhashDeleteFile(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	project, filePath, err := db2contract.DecodeMinhashDeleteFileRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, err := tx.Exec(ctx, minhashDeleteFileSignatureQuery, project, filePath); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, minhashDeleteFileBucketsQuery, project, filePath)
		return err
	})
	return acknowledgement(txErr == nil, db2contract.EncodeMinhashDeleteFileReply)
}

// The basename is extracted in SQL rather than by scanning every file, which is
// what the C does: it reads every path in the project's current generation and
// compares in C. regexp_replace strips everything up to the last slash, which
// is exactly strrchr's answer, and a path with no slash keeps its whole self.
//
// LIMIT 2 because the question is "is there exactly one", not "what are they".
// A second row is all it takes to answer no.
const uniqueFileBasenameQuery = `SELECT f.path FROM files f
 JOIN projects p ON p.id = f.project_id
 WHERE p.name = $1 AND p.lifecycle_state = 'current'
 AND f.generation = p.current_generation
 AND regexp_replace(f.path, '^.*/', '') = $2
 ORDER BY f.path LIMIT 2`

// uniqueFileBasename resolves a bare filename to its path, when only one file
// carries that name.
//
// Ambiguity answers empty, not a guess. The caller is resolving a name a person
// typed, and two candidates mean the name did not identify a file -- returning
// the first would be picking one on their behalf without saying so.
func uniqueFileBasename(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, basename, err := db2contract.DecodeUniqueFileBasenameRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	paths, status := readTextColumn(ctx, store, 2, uniqueFileBasenameQuery, project, basename)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	unique := ""
	if len(paths) == 1 {
		unique = paths[0]
	}
	reply, err := db2contract.EncodeUniqueFileBasenameReply(unique)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const entityEdgeBumpUtilityQuery = `UPDATE entity_edges
 SET utility_score = GREATEST(-5.0, LEAST(5.0, utility_score + $2)),
     utility_touched_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')
 WHERE source = $1 OR target = $1`

// entityEdgeBumpUtility moves the usefulness score of every edge touching an
// entity.
//
// Every edge on either side, not one edge: the caller has observed that an
// entity was useful, and the evidence is about the entity rather than about a
// particular relation it appears in. An entity at the centre of many edges
// therefore moves them all, which is what makes the score converge on how
// often the entity earns its place.
//
// Clamped in the statement to [-5, 5]. Doing it in SQL rather than by reading
// and writing back is what makes concurrent bumps safe: two callers each add
// their delta to whatever is there, and neither can push past the bound.
func entityEdgeBumpUtility(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, delta, err := db2contract.DecodeEntityEdgeBumpUtilityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, entityEdgeBumpUtilityQuery, entity, delta)
	return acknowledgement(execErr == nil, db2contract.EncodeEntityEdgeBumpUtilityReply)
}

// One statement, where the C reads the payload, parses it, merges in C and
// writes it back inside a transaction.
//
// The jsonb || operator merges two objects with the right side winning, which
// is exactly what the C's delete-then-add produces. The CASE covers a payload
// that is absent or is not an object: the C replaces it outright, on the
// grounds that the flag is the point of the call and a non-object payload has
// nothing for the flag to merge into.
//
// The C's transaction exists to protect its read-modify-write. One statement
// removes the need for it and closes a race it could not: two concurrent flags
// there can interleave between the SELECT and the UPDATE, and the later write
// silently discards whatever the earlier one merged. Here they serialize on the
// row.
const artifactFlagReviewQuery = `UPDATE artifacts
 SET state = 'proposed',
     payload = (CASE WHEN jsonb_typeof(payload) = 'object'
                     THEN payload ELSE '{}'::jsonb END)
               || jsonb_build_object('flagged_for_review', true, 'flagged_reason', $2::text)
 WHERE id = $1`

// artifactFlagReview marks an artifact as needing a person to look at it.
//
// It also returns the artifact to 'proposed', so flagging a committed artifact
// un-commits it. That is the point rather than a side effect: something under
// review is not something in force.
//
// An empty reason becomes "flagged", because the column is what a reviewer
// reads and an empty string tells them nothing about why they are here.
//
// Requires the artifact to exist. The C rolls back when its SELECT finds no
// row, and answering acknowledged for an artifact nobody holds would tell a
// caller their flag landed somewhere.
func artifactFlagReview(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, reason, err := db2contract.DecodeArtifactFlagReviewRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if reason == "" {
		reason = "flagged"
	}
	changed, execErr := store.Exec(ctx, artifactFlagReviewQuery, artifactID, reason)
	return acknowledgement(execErr == nil && changed > 0,
		db2contract.EncodeArtifactFlagReviewReply)
}

const verdictSuppressedQuery = `SELECT COUNT(*) FROM audit_events
 WHERE verdict = 'thumbs_down' AND verdict_tag = $1 AND verdict_scope = $2`

// verdictSuppressed reports whether a kind of suggestion has been rejected in a
// scope before.
//
// One thumbs-down suppresses. There is no threshold and no decay: a person who
// said no once to this tag in this scope is not asked again, which is a
// deliberate asymmetry -- the cost of re-suggesting something already refused
// is higher than the cost of missing one they might now accept.
//
// A flag rather than the count, so a caller cannot start treating "how many"
// as a strength. The count exists only to be compared against zero.
func verdictSuppressed(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	tag, scope, err := db2contract.DecodeVerdictSuppressedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count, status := readOptionalInt(ctx, store, verdictSuppressedQuery, tag, scope)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	suppressed := uint32(0)
	if count > 0 {
		suppressed = 1
	}
	reply, err := db2contract.EncodeVerdictSuppressedReply(suppressed)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

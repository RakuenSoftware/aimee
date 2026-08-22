package db2

import (
	"context"
	"errors"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageProjectStats,
		db2contract.OperationProjectStats, projectStats)
	Register(db2contract.StagePurgeFilesMatching,
		db2contract.OperationPurgeFilesMatching, purgeFilesMatching)
	Register(db2contract.StageProposalsSettledCounts,
		db2contract.OperationProposalsSettledCounts, proposalsSettledCounts)
	Register(db2contract.StageRetrievalEventByTurn,
		db2contract.OperationRetrievalEventByTurn, retrievalEventByTurn)
	Register(db2contract.StageArtifactLinksRead,
		db2contract.OperationArtifactLinksRead, artifactLinksRead)
	Register(db2contract.StageCorpusPipelineStageCounts,
		db2contract.OperationCorpusPipelineStageCounts, corpusPipelineStageCounts)
}

// Both counts exclude dotfiles, dot-directories anywhere in the path, and any
// project whose root itself sits under one. These are the numbers shown to a
// person as "what is indexed", and a .git directory is not something they put
// there -- counting it would make every project look larger than the work in
// it.
//
// The two subqueries repeat the exclusion rather than sharing it, because they
// count different things over the same files and a single scan returning both
// would need a join that changes what COUNT(*) means.
const projectStatsQuery = `SELECT
 (SELECT COUNT(*) FROM files f
  JOIN projects p ON p.id = f.project_id
  WHERE p.name = $1
    AND p.lifecycle_state = 'current'
    AND f.generation = p.current_generation
    AND f.path NOT LIKE '.%'
    AND f.path NOT LIKE '%/.%'
    AND p.root NOT LIKE '%/.%'),
 (SELECT COUNT(*) FROM terms t
  JOIN files f ON f.id = t.file_id
  JOIN projects p ON p.id = f.project_id
  WHERE p.name = $1 AND t.kind = 'definition'
    AND p.lifecycle_state = 'current'
    AND f.generation = p.current_generation
    AND f.path NOT LIKE '.%'
    AND f.path NOT LIKE '%/.%'
    AND p.root NOT LIKE '%/.%')`

// projectStats reports how many files and definitions a project currently has
// indexed.
//
// A project that is not indexed answers zero and zero rather than failing: the
// caller is asking how much is there, and none is an answer.
func projectStats(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	project, err := db2contract.DecodeProjectStatsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var files, definitions *int64
	if scanErr := store.QueryRow(ctx, projectStatsQuery, project).
		Scan(&files, &definitions); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProjectStatsReply(
		clampToU32(number(files)), clampToU32(number(definitions)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const purgeFilesMatchingQuery = `DELETE FROM files
 WHERE project_id = $1 AND path LIKE $2
 AND generation = (SELECT current_generation FROM projects WHERE id = $1)`

// purgeFilesMatching removes indexed files whose path matches a pattern.
//
// The pattern is a LIKE pattern passed through untouched, which is the point:
// the caller is purging a directory or an extension and needs its wildcards.
// A pattern of '%' would empty the project's current generation, and nothing
// here stops that -- the C does not either, and a purge that refused its own
// widest case would need a rule about what counts as too wide.
//
// Scoped to the current generation, so a purge cannot reach a published one.
// Note that this resolves the generation by project id without checking
// lifecycle_state, unlike its neighbours: a detached project still has a
// current_generation and its files are still purgeable, which is what lets a
// project be cleaned up after it is detached.
func purgeFilesMatching(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	projectID, pattern, err := db2contract.DecodePurgeFilesMatchingRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, purgeFilesMatchingQuery, int64(projectID), pattern)
	if execErr != nil {
		deleted = 0
	}
	reply, err := db2contract.EncodePurgeFilesMatchingReply(clampToU32(deleted))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// NULLIF on the first two columns because all three are NOT NULL with an
// empty-string default: a bare COALESCE always returns committed_at, which is
// empty for anything not committed, and the empty string never satisfies the
// window comparison. Every archived proposal was excluded, so the terminal
// count could not differ from the committed one.
const proposalsSettledCountsQuery = `SELECT
 SUM(CASE WHEN state = 'committed' THEN 1 ELSE 0 END),
 SUM(CASE WHEN state IN ('committed', 'archived') THEN 1 ELSE 0 END)
 FROM learning_proposals
 WHERE COALESCE(NULLIF(committed_at, ''), NULLIF(updated_at, ''), created_at)
       >= pg_now_text($1)`

// proposalsSettledCounts reports how many proposals were settled in a window.
//
// Committed and terminal, where terminal is committed plus archived, so the
// difference between them is how much was decided against. That difference is
// the number the pair exists to expose.
//
// SUM over an empty set is NULL rather than zero, which is the nullable-column
// trap by another route: the columns are integers and the aggregate still
// answers NULL when nothing matched.
func proposalsSettledCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	windowDays, err := db2contract.DecodeProposalsSettledCountsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The window is an interval expression rather than a number, which is what
	// pg_now_text takes. It is built from an integer the contract has already
	// bounded, so there is nothing of the caller's in the string but a count.
	window := fmt.Sprintf("-%d days", windowDays)

	var committed, terminal *int64
	if scanErr := store.QueryRow(ctx, proposalsSettledCountsQuery, window).
		Scan(&committed, &terminal); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProposalsSettledCountsReply(
		clampToU64(number(committed)), clampToU64(number(terminal)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const retrievalEventByTurnQuery = `SELECT id, payload FROM artifacts
 WHERE kind = 'retrieval_event' AND turn_id = $1 LIMIT 1`

// retrievalEventByTurn finds the retrieval an answer was assembled from.
//
// LIMIT 1 with no ordering, because a turn is expected to have at most one
// retrieval event. Two would mean the same turn was answered twice, and which
// of them comes back is unstated rather than chosen.
//
// Both fields empty means no event, which is ordinary: a turn that answered
// without retrieving has none.
func retrievalEventByTurn(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	turnID, err := db2contract.DecodeRetrievalEventByTurnRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var eventID, payload *string
	if scanErr := store.QueryRow(ctx, retrievalEventByTurnQuery, turnID).
		Scan(&eventID, &payload); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
	}
	reply, err := db2contract.EncodeRetrievalEventByTurnReply(text(eventID), text(payload))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const artifactLinksReadQuery = `SELECT to_id, kind FROM artifact_links WHERE from_id = $1`

// artifactLinksRead lists what an artifact points at.
//
// One direction only: this answers "what does this cite or supersede", not
// "what points here". A caller wanting the reverse has to ask the other way
// round, and there is no operation that does -- worth knowing before assuming
// the graph is walkable in both directions.
func artifactLinksRead(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	artifactID, err := db2contract.DecodeArtifactLinksReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ArtifactLinksReadMaxRows
	rows, queryErr := store.Query(ctx, artifactLinksReadQuery, artifactID)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.ArtifactLinksReadRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var target, kind *string
		if err := rows.Scan(&target, &kind); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.ArtifactLinksReadRow{
			ToArtifactID: text(target),
			LinkKind:     text(kind),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeArtifactLinksReadReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const corpusPipelineStageCountsQuery = `SELECT stage, stage_status, COUNT(*)
 FROM corpus_processing_jobs
 GROUP BY stage, stage_status ORDER BY stage, stage_status`

// corpusPipelineStageCounts reports how many jobs sit at each stage and status.
//
// Every stage and status pair that has at least one job, so a stage nothing has
// reached is absent rather than zero. A caller rendering this has to treat a
// missing pair as none -- there is no list of the stages that exist to fill in
// against.
func corpusPipelineStageCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeCorpusPipelineStageCountsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CorpusPipelineStageCountsMaxRows
	rows, queryErr := store.Query(ctx, corpusPipelineStageCountsQuery)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.CorpusPipelineStageCountsRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var stage, status *string
		var count *int64
		if err := rows.Scan(&stage, &status, &count); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.CorpusPipelineStageCountsRow{
			Stage:       text(stage),
			StageStatus: text(status),
			JobCount:    clampToU32(number(count)),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeCorpusPipelineStageCountsReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

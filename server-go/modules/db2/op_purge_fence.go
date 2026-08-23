package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StagePurgeFenceHeartbeat,
		db2contract.OperationPurgeFenceHeartbeat, purgeFenceHeartbeat)
	Register(db2contract.StagePurgeFenceClear,
		db2contract.OperationPurgeFenceClear, purgeFenceClear)
	Register(db2contract.StageDocumentHashExists,
		db2contract.OperationDocumentHashExists, documentHashExists)
	Register(db2contract.StageMemorySummariesList,
		db2contract.OperationMemorySummariesList, memorySummariesList)
	Register(db2contract.StageMemoryL1SessionClusters,
		db2contract.OperationMemoryL1SessionClusters, memoryL1SessionClusters)
	Register(db2contract.StageMemoryArtifactHashedList,
		db2contract.OperationMemoryArtifactHashedList, memoryArtifactHashedList)
}

// The fence is two rows keyed by project: an identity row holding
// "<generation> <purge_id>", and a heartbeat row holding when it was last
// touched. Both prefixes are the C's and a caller reading the fence any other
// way would not find it.
const (
	purgeFenceKeyPrefix   = "project_purging:"
	purgeFenceTSKeyPrefix = "project_purging_ts:"
)

// The advisory lock comes first and the row lock second, in that order.
//
// The C is explicit about why a conditional UPDATE with an EXISTS predicate is
// not enough: under READ COMMITTED, row re-evaluation can act on an old
// snapshot, so a heartbeat could refresh a fence another purge had already
// taken. Taking the project lock, then locking the identity row FOR UPDATE, and
// only then comparing, is what serialises the decision against the mutation.
//
// The lock is transaction-scoped, so it is released by the commit or rollback
// rather than needing its own unlock -- which is what keeps a failure from
// leaving a project fenced off for good.
const (
	// The lock call is wrapped so the row carries an integer. pg_advisory_xact_lock
	// returns void, which is not a type pgx knows how to scan -- the C never had
	// to care because libpq hands back text either way.
	purgeFenceGuardQuery = `SELECT 1 FROM
 (SELECT pg_advisory_xact_lock(hashtext('aimee_purge:' || $1))) AS project_lock`
	purgeFenceMatchQuery = `SELECT state_value FROM kb_runtime_state
 WHERE state_key = $1 FOR UPDATE`
	// pg_now_text() rather than a to_char of CURRENT_TIMESTAMP: the heartbeat is
	// read back by a liveness check that casts the text to a timestamp and
	// compares it against now in UTC, so a value written in the server's local
	// zone would make the fence read as stale or live by the size of the offset.
	// The format is the schema's canonical one, and every reader in the tree
	// expects it.
	purgeFenceTouchQuery = `INSERT INTO kb_runtime_state (state_key, state_value)
 VALUES ($1, pg_now_text())
 ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`
	purgeFenceDeleteQuery = `DELETE FROM kb_runtime_state WHERE state_key = $1`
)

// errFenceNotOurs rolls the transaction back without treating a mismatch as a
// failure. The fence being absent and the fence belonging to another purge are
// the same answer to this caller: it does not hold the project.
var errFenceNotOurs = errors.New("db2: the fence is absent or held by another purge")

// mutateMatchedFence serves both the heartbeat and the clear.
//
// Answering false covers both "no fence" and "someone else's fence", which the
// caller cannot tell apart -- and does not need to, because either way this
// purge is not the one that owns the project.
func mutateMatchedFence(ctx context.Context, store Store,
	project, generation, purgeID string, clear bool,
) bool {
	key := purgeFenceKeyPrefix + project
	timestampKey := purgeFenceTSKeyPrefix + project
	expected := generation + " " + purgeID

	matched := false
	txErr := store.InTx(ctx, func(tx Store) error {
		var locked *int64
		if err := tx.QueryRow(ctx, purgeFenceGuardQuery, project).Scan(&locked); err != nil {
			return err
		}
		var current *string
		if err := tx.QueryRow(ctx, purgeFenceMatchQuery, key).Scan(&current); err != nil {
			// No fence row at all: nothing to heartbeat and nothing to clear.
			return errFenceNotOurs
		}
		if text(current) != expected {
			return errFenceNotOurs
		}
		if !clear {
			_, err := tx.Exec(ctx, purgeFenceTouchQuery, timestampKey)
			if err == nil {
				matched = true
			}
			return err
		}
		// Identity row first when clearing, which is the reverse of the write
		// order: a torn clear then leaves an orphan heartbeat, which is
		// harmless without an identity row, rather than an identity row with no
		// heartbeat -- a fence that looks held by nobody and never expires.
		if _, err := tx.Exec(ctx, purgeFenceDeleteQuery, key); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, purgeFenceDeleteQuery, timestampKey); err != nil {
			return err
		}
		matched = true
		return nil
	})
	return txErr == nil && matched
}

// purgeFenceHeartbeat refreshes the fence, proving the purge still holds it.
func purgeFenceHeartbeat(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, generation, purgeID, err :=
		db2contract.DecodePurgeFenceHeartbeatRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	applied := mutateMatchedFence(ctx, store, project, generation, purgeID, false)
	return acknowledgement(applied, db2contract.EncodePurgeFenceHeartbeatReply)
}

// purgeFenceClear releases the fence when the purge that took it is finished.
//
// Both rows go, and only when the caller's generation and purge identifier
// match what is held. A purge that has already been taken over clears nothing,
// which is what stops a slow purge from releasing the fence out from under the
// one that displaced it.
func purgeFenceClear(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, generation, purgeID, err :=
		db2contract.DecodePurgeFenceClearRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	applied := mutateMatchedFence(ctx, store, project, generation, purgeID, true)
	return acknowledgement(applied, db2contract.EncodePurgeFenceClearReply)
}

// UNION rather than UNION ALL, because the same path can appear in both tables
// and the caller wants one sample rather than a count. Both halves pin the
// generation, so a hash that matched in a superseded generation does not read
// as already ingested.
const documentHashExistsQuery = `SELECT d.file_path FROM kb_documents d
 JOIN projects p ON p.name = d.project
 WHERE d.project = $1 AND d.file_hash = $2
 AND p.lifecycle_state = 'current' AND d.generation = p.current_generation
 UNION
 SELECT k.file_path FROM kb_file_index k
 JOIN projects p2 ON p2.name = k.project
 WHERE k.project = $1 AND k.file_hash = $2
 AND p2.lifecycle_state = 'current' AND k.generation = p2.current_generation
 LIMIT 1`

// documentHashExists reports whether a file's content is already ingested, and
// where.
//
// Two tables because a file can be indexed without being chunked into
// documents: kb_file_index knows the file, kb_documents knows its chunks, and
// either is enough to say the content is present. The sample path comes back so
// a caller reporting a duplicate can name the file it duplicates.
func documentHashExists(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, fileHash, err := db2contract.DecodeDocumentHashExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	path, status := readOptionalText(ctx, store, documentHashExistsQuery, project, fileHash)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	exists := uint32(0)
	if path != "" {
		exists = 1
	}
	reply, err := db2contract.EncodeDocumentHashExistsReply(exists, path)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memorySummariesListQuery = `SELECT scope, summary FROM memory_summaries
 WHERE memory_id = $1 ORDER BY id ASC LIMIT $2`

// memorySummariesList lists the summaries written for a memory, oldest first.
//
// Ascending, unlike most reads here. A memory accumulates summaries at
// different scopes and reading them in the order they were written is what
// makes the sequence legible -- a caller assembling context wants the
// narrowest, earliest framing first rather than the latest rewrite.
func memorySummariesList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, limit, err := db2contract.DecodeMemorySummariesListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := int(limit)
	if ceiling <= 0 || ceiling > db2contract.MemorySummariesListMaxRows {
		ceiling = db2contract.MemorySummariesListMaxRows
	}
	rows, queryErr := store.Query(ctx, memorySummariesListQuery, int64(memoryID), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemorySummariesListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var scope, summary *string
		if err := rows.Scan(&scope, &summary); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemorySummariesListRow{
			SummaryScope: text(scope),
			SummaryText:  text(summary),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemorySummariesListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryL1SessionClustersQuery = `SELECT source_session, COUNT(*)
 FROM memories
 WHERE tier = 'L1' AND source_session != '' AND source_session != $1
 GROUP BY source_session
 HAVING COUNT(*) >= $2`

// memoryL1SessionClusters finds sessions that produced enough scratch memories
// to be worth consolidating.
//
// The excluded session is the caller's own: a session does not consolidate
// itself while it is still running. Sessions with no identifier are excluded
// too, because they are not a session -- grouping them would pool every
// untracked memory into one enormous phantom cluster.
//
// No ordering, so a caller wanting the largest sorts for itself.
func memoryL1SessionClusters(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	excluded, minimum, err := db2contract.DecodeMemoryL1SessionClustersRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryL1SessionClustersMaxRows
	rows, queryErr := store.Query(ctx, memoryL1SessionClustersQuery, excluded, int64(minimum))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryL1SessionClustersRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var session *string
		var count *int64
		if err := rows.Scan(&session, &count); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryL1SessionClustersRow{
			SessionID:    text(session),
			ClusterCount: clampToU32(number(count)),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryL1SessionClustersReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The artifact columns live on memories rather than in a table of their own, so
// this is a memory carrying an artifact reference rather than an artifact
// carrying a memory. Both columns are nullable, and IS NOT NULL is the filter:
// a memory with no artifact is not a row here at all.
const memoryArtifactHashedListQuery = `SELECT id, artifact_type, artifact_ref, artifact_hash
 FROM memories
 WHERE artifact_type IS NOT NULL AND artifact_hash IS NOT NULL
 LIMIT $1`

// memoryArtifactHashedList lists memories that point at a hashed artifact.
//
// Only hashed ones, because the hash is what a verification pass compares
// against: a reference with no hash cannot be checked for drift, so including
// it would put rows in front of a caller that it can do nothing with.
//
// artifact_ref is not required to be present -- only the type and the hash are
// -- so a row can come back naming what it is and what it hashed to without
// saying where it lives.
func memoryArtifactHashedList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeMemoryArtifactHashedListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryArtifactHashedListMaxRows
	rows, queryErr := store.Query(ctx, memoryArtifactHashedListQuery, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.MemoryArtifactHashedListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var memoryID *int64
		var artifactType, artifactRef, artifactHash *string
		if err := rows.Scan(&memoryID, &artifactType, &artifactRef,
			&artifactHash); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.MemoryArtifactHashedListRow{
			MemoryID:     clampToU64(number(memoryID)),
			ArtifactType: text(artifactType),
			ArtifactRef:  text(artifactRef),
			ArtifactHash: text(artifactHash),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryArtifactHashedListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

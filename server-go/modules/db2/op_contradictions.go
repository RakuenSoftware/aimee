package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageLifecycleUnresolvedContradictions,
		db2contract.OperationLifecycleUnresolvedContradictions,
		lifecycleUnresolvedContradictions)
}

// LEFT JOIN on both sides, so a conflict whose memory has been deleted still
// has a row -- and then the scope filter decides whether it survives. An INNER
// JOIN would drop it here for the wrong reason, and the two cases want
// different answers.
//
// b_key is selected by the C and never read: the reply carries a_key, both
// contents, and no key for the second side. Dropped rather than selected and
// discarded.
const unresolvedContradictionsBody = `SELECT c.id, c.memory_a, c.memory_b,
 c.detected_at, ma.key, ma.content, mb.content
 FROM memory_conflicts c
 LEFT JOIN memories ma ON ma.id = c.memory_a
 LEFT JOIN memories mb ON mb.id = c.memory_b
 WHERE c.resolved = 0`

// lifecycleUnresolvedContradictions lists contradictions the caller can see
// both sides of.
//
// Scope-filtered on both memories, and that is the point of the operation's
// shape: the reply exposes one key and both contents, so a conflict with one
// side out of scope would leak that side's text to a caller who cannot see the
// memory. Suppressing the whole row is the C's answer and it is the right one
// -- a partial contradiction is not an alert anyone can act on.
//
// The ordering takes the higher of the two ranks, so a conflict touching the
// caller's own project leads even when its other side is merely shared. Within
// a rank the most recently detected comes first.
//
// This operation reached a scope-filtered statement while the catalogue
// recorded it as unscoped, because its C wraps the scope macro in file-local
// aliases and the generator's scan looks for the macro by name. The scan now
// follows aliases; without the fix this would have been ported as an unscoped
// read, which under an inactive scope means every row rather than none.
func lifecycleUnresolvedContradictions(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	flags, workspace, project, err :=
		db2contract.DecodeLifecycleUnresolvedContradictionsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	scope := DecodeScope(flags, workspace, project)
	filterA, scopeArgs := scope.filter("ma.id", scopedLimitPlaceholder)
	filterB := scope.filterSharing("mb.id", scopedLimitPlaceholder)
	rankA := scope.rankExpression("ma.id", scopedLimitPlaceholder)
	rankB := scope.rankExpression("mb.id", scopedLimitPlaceholder)
	statement := unresolvedContradictionsBody + filterA + filterB +
		` ORDER BY CASE WHEN ` + rankA + ` > ` + rankB +
		` THEN ` + rankA + ` ELSE ` + rankB + ` END DESC,` +
		` c.detected_at DESC LIMIT $1`

	ceiling := db2contract.LifecycleUnresolvedContradictionsMaxRows
	rows, queryErr := store.Query(ctx, statement, scopedArgs(ceiling, scopeArgs)...)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.LifecycleUnresolvedContradictionsRow, 0, 16)
	for rows.Next() && len(found) < ceiling {
		var conflictID, memoryA, memoryB int64
		var detectedAt string
		// The joined columns are nullable twice over: the join is outer, and
		// memories.source-side columns can be absent in their own right.
		var keyA, contentA, contentB *string
		if scanErr := rows.Scan(&conflictID, &memoryA, &memoryB, &detectedAt,
			&keyA, &contentA, &contentB); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.LifecycleUnresolvedContradictionsRow{
			ConflictID: clampToU64(conflictID),
			MemoryAID:  clampToU64(memoryA),
			MemoryBID:  clampToU64(memoryB),
			DetectedAt: detectedAt,
			AKey:       text(keyA),
			AContent:   text(contentA),
			BContent:   text(contentB),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr :=
		db2contract.EncodeLifecycleUnresolvedContradictionsReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

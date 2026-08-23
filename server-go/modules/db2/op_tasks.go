package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageTaskCreate,
		db2contract.OperationTaskCreate, taskCreate)
	Register(db2contract.StageTaskAddEdge,
		db2contract.OperationTaskAddEdge, taskAddEdge)
	Register(db2contract.StageToolRegistryLookup,
		db2contract.OperationToolRegistryLookup, toolRegistryLookup)
	Register(db2contract.StageRecomputeBlockedSymbols,
		db2contract.OperationRecomputeBlockedSymbols, recomputeBlockedSymbols)
}

// The state and the confidence are fixed at creation: every task starts as
// something nobody has done yet and nothing has cast doubt on. The C writes the
// same two literals.
const taskCreateQuery = `INSERT INTO tasks
 (parent_id, title, state, confidence, session_id, created_at, updated_at)
 VALUES ($1, $2, 'todo', 1.0, $3, pg_now_text(), pg_now_text())
 RETURNING id`

// taskCreate opens a task and answers its identifier.
//
// A parent of zero means no parent rather than task zero, which is the
// convention the column's own default carries -- so a task created without one
// is a root and reads as such without a NULL to handle.
//
// The two timestamps come from one pg_now_text() call each rather than a value
// computed once and bound twice, as the C does. Within a statement the function
// is stable, so both columns still get the same instant.
func taskCreate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	title, sessionID, parentID, err := db2contract.DecodeTaskCreateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var taskID int64
	if scanErr := store.QueryRow(ctx, taskCreateQuery,
		int64(parentID), title, sessionID).Scan(&taskID); scanErr != nil {
		// Zero is the reply for a task that was not created, which is what the
		// adapter answers for the C's failure return.
		taskID = 0
	}
	if taskID < 0 {
		taskID = 0
	}
	reply, encodeErr := db2contract.EncodeTaskCreateReply(uint64(taskID))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// No ON CONFLICT: the same edge added twice is two rows, because task_edges has
// no uniqueness constraint to collide with. That is the C's behaviour and the
// table's, and adding one here would be a schema decision dressed up as a port.
const taskAddEdgeQuery = `INSERT INTO task_edges (source_id, target_id, relation)
 VALUES ($1, $2, $3)`

// taskAddEdge records a relation between two tasks.
//
// Both ends are foreign keys into tasks, so an edge naming a task that does not
// exist fails rather than dangling -- and this answers unacknowledged, which is
// the one thing the reply can usefully say.
func taskAddEdge(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	source, target, relation, err := db2contract.DecodeTaskAddEdgeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, taskAddEdgeQuery,
		int64(source), int64(target), relation)
	return acknowledgement(execErr == nil, db2contract.EncodeTaskAddEdgeReply)
}

const toolRegistryLookupQuery = `SELECT input_schema, side_effect, enabled
 FROM tool_registry WHERE name = $1`

// toolRegistryLookup answers what a tool takes, what it does to the world, and
// whether it is currently allowed to run.
//
// The side effect matters more than it looks: it is what a caller consults
// before running a tool speculatively. The C substitutes "read" for a NULL
// value -- the safest of the possible answers -- but the column is declared NOT
// NULL with that same default, so the substitution never happens and a tool
// registered without one already reads as a read.
//
// A tool that is not registered is not found rather than found-and-disabled.
// The difference is that a caller can register the first and cannot enable the
// second.
func toolRegistryLookup(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	name, err := db2contract.DecodeToolRegistryLookupRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var inputSchema, sideEffect string
	var enabled int64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, toolRegistryLookupQuery, name).
		Scan(&inputSchema, &sideEffect, &enabled); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, inputSchema, sideEffect, enabled = 0, "", "", 0
	}
	enabledFlag := uint32(0)
	if enabled != 0 {
		enabledFlag = 1
	}
	reply, encodeErr := db2contract.EncodeToolRegistryLookupReply(
		found, inputSchema, sideEffect, enabledFlag)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The version is bumped inside the transaction with an UPDATE, then read back,
// rather than read before it and incremented. The C carries the reasoning: the
// UPDATE takes the row lock, so two concurrent recomputes serialise and cannot
// both commit the same version or step it backwards.
//
// RETURNING does what the C's UPDATE-then-SELECT does in one statement. The C
// avoids it deliberately, for a sqlite shim that lacks it; nothing here talks to
// sqlite, and the round trip it saves is also a row lock held for less time.
const (
	blockedSymbolsBumpQuery = `UPDATE cross_repo_meta
 SET blocked_symbols_version = blocked_symbols_version + 1
 WHERE id = 1 RETURNING blocked_symbols_version`
	blockedSymbolsClearQuery = `DELETE FROM blocked_symbols`
	// Two arms unioned, both over trusted repos of the current generation. The
	// definition arm matches kind = 'definition' positively rather than
	// excluding the kinds it does not want, so a kind added later cannot
	// silently inflate the count.
	//
	// UNION rather than UNION ALL, and that is what makes the insert safe
	// without an ON CONFLICT: a symbol that is both widely called and widely
	// defined appears once, so the two arms cannot produce duplicate rows into a
	// table the same transaction has just emptied.
	blockedSymbolsFillQuery = `INSERT INTO blocked_symbols (word, lang, reason, version)
 SELECT name, '', 'frequency', $3 FROM (
   SELECT cc.callee AS name FROM code_calls cc
     JOIN files f ON f.id = cc.file_id JOIN projects p ON p.id = f.project_id
     WHERE p.trust = 'trusted' AND p.lifecycle_state = 'current'
       AND f.generation = p.current_generation AND length(cc.callee) >= $4
     GROUP BY cc.callee HAVING COUNT(DISTINCT p.id) >= $1
   UNION
   SELECT t.name AS name FROM terms t
     JOIN files f ON f.id = t.file_id JOIN projects p ON p.id = f.project_id
     WHERE p.trust = 'trusted' AND p.lifecycle_state = 'current'
       AND f.generation = p.current_generation
       AND t.kind = 'definition' AND length(t.name) >= $4
     GROUP BY t.name HAVING COUNT(DISTINCT p.id) >= $2
 ) q`
)

// recomputeBlockedSymbols rebuilds the list of symbols too common to be worth
// matching across repositories, and answers how many there now are.
//
// Clear-and-refill inside one transaction, so no reader ever sees a partial
// list: the whole point of the table is that a lookup against it is cheap and
// complete, and a half-filled one would let symbols through for as long as the
// refill took.
//
// The count is the number of rows this recompute inserted, taken inside the
// transaction. The C counts the table again after committing, which can see a
// concurrent recompute's rows instead of its own -- the same serialisation the
// version bump exists to prevent, undone one statement later.
func recomputeBlockedSymbols(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	calleeRepoMin, definitionRepoMin, symbolLengthMin, err :=
		db2contract.DecodeRecomputeBlockedSymbolsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var blocked int64
	txErr := store.InTx(ctx, func(tx Store) error {
		var version int64
		// No meta row means no version to bump, and inventing one here would
		// hand out a version another recompute may already have used. The C
		// fails the same way.
		if err := tx.QueryRow(ctx, blockedSymbolsBumpQuery).Scan(&version); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, blockedSymbolsClearQuery); err != nil {
			return err
		}
		inserted, err := tx.Exec(ctx, blockedSymbolsFillQuery,
			int64(calleeRepoMin), int64(definitionRepoMin), version,
			int64(symbolLengthMin))
		if err != nil {
			return err
		}
		blocked = inserted
		return nil
	})
	if txErr != nil {
		// Zero blocked symbols, which is what the adapter answers for the C's
		// failure return. It is indistinguishable from a recompute that found
		// nothing, and neither the C nor the envelope offers anywhere to say
		// which happened.
		blocked = 0
	}
	reply, encodeErr := db2contract.EncodeRecomputeBlockedSymbolsReply(uint32(blocked))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

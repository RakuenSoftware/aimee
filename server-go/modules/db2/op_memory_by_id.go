package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageHasWorkspaceTag,
		db2contract.OperationHasWorkspaceTag, hasWorkspaceTag)
	Register(db2contract.StageDeleteRow,
		db2contract.OperationDeleteRow, deleteRow)
	Register(db2contract.StageTouch, db2contract.OperationTouch, touch)
	Register(db2contract.StageLinkDelete,
		db2contract.OperationLinkDelete, linkDelete)
	Register(db2contract.StageValidAt, db2contract.OperationValidAt, validAt)
	Register(db2contract.StageHasScopeType,
		db2contract.OperationHasScopeType, hasScopeType)
	Register(db2contract.StageReject, db2contract.OperationReject, reject)
	Register(db2contract.StageUpdateContent,
		db2contract.OperationUpdateContent, updateContent)
	Register(db2contract.StageDecayConfidence,
		db2contract.OperationDecayConfidence, decayConfidence)
	Register(db2contract.StageWorkspaceTagInsert,
		db2contract.OperationWorkspaceTagInsert, workspaceTagInsert)
	Register(db2contract.StageSetCognifiedKind,
		db2contract.OperationSetCognifiedKind, setCognifiedKind)
	Register(db2contract.StageSetSourceSession,
		db2contract.OperationSetSourceSession, setSourceSession)
	Register(db2contract.StageNegationTokensUpdate,
		db2contract.OperationNegationTokensUpdate, negationTokensUpdate)
	Register(db2contract.StageRecordExists,
		db2contract.OperationRecordExists, recordExists)
}

const (
	hasWorkspaceTagQuery = `SELECT EXISTS (
 SELECT 1 FROM memory_workspaces WHERE memory_id = $1)`

	deleteRowQuery = `DELETE FROM memories WHERE id = $1`

	touchQuery = `UPDATE memories
 SET use_count = use_count + 1, last_used_at = pg_now_text() WHERE id = $1`

	linkDeleteQuery = `DELETE FROM memory_links WHERE id = $1`

	hasScopeTypeQuery = `SELECT EXISTS (
 SELECT 1 FROM memory_scopes WHERE memory_id = $1 AND scope_type = $2)`

	// Confidence never goes below zero, and the floor is in the statement so a
	// memory rejected enough times settles at "no confidence" rather than
	// crossing into a negative one nothing knows how to read.
	rejectQuery = `UPDATE memories
 SET confidence = GREATEST(confidence - 0.1, 0.0), updated_at = pg_now_text()
 WHERE id = $1`

	updateContentQuery = `UPDATE memories
 SET content = $2, updated_at = pg_now_text() WHERE id = $1`

	// Decay multiplies where reject subtracts. The two are different verbs: a
	// rejection is evidence against this memory, and decay is time passing.
	decayConfidenceQuery = `UPDATE memories
 SET confidence = confidence * 0.7 WHERE id = $1`

	workspaceTagInsertQuery = `INSERT INTO memory_workspaces (memory_id, workspace)
 VALUES ($1, $2) ON CONFLICT DO NOTHING`

	setCognifiedKindQuery = `UPDATE memories
 SET cognified_memory_kind = $2 WHERE id = $1`

	setSourceSessionQuery = `UPDATE memories SET source_session = $2 WHERE id = $1`

	negationTokensUpdateQuery = `UPDATE memories
 SET negation_tokens = $2 WHERE id = $1`

	// A record is either a memory or one of its units, and the caller asking
	// does not know which -- the identifier spaces are separate but the
	// question is "is there anything under this id".
	recordExistsQuery = `SELECT EXISTS (SELECT 1 FROM memories WHERE id = $1)
    OR EXISTS (SELECT 1 FROM memory_units WHERE id = $1)`

	// The comparison the C spells out and this keeps: stamps are text, both
	// spellings of the separator occur in the column, and an empty bound means
	// unbounded rather than "the epoch". NULLIF turns the empty string into the
	// null the IS NULL test wants; replace and rtrim make a 'T'-separated
	// Zulu stamp compare equal to a space-separated one.
	validAtQuery = `SELECT EXISTS (
 SELECT 1 FROM memories WHERE id = $1
   AND (NULLIF(valid_from, '') IS NULL
        OR rtrim(replace(valid_from, 'T', ' '), 'Z')
           <= rtrim(replace($2, 'T', ' '), 'Z'))
   AND (NULLIF(valid_until, '') IS NULL
        OR rtrim(replace(valid_until, 'T', ' '), 'Z')
           > rtrim(replace($2, 'T', ' '), 'Z')))`
)

func hasWorkspaceTag(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeHasWorkspaceTagRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, hasWorkspaceTagQuery,
		db2contract.EncodeHasWorkspaceTagReply, int64(memoryID))
}

// deleteRow removes one memory and answers how many rows that was, which is
// one or none -- the identifier is the primary key.
func deleteRow(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeDeleteRowRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, deleteRowQuery, int64(memoryID))
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(deleted, db2contract.EncodeDeleteRowReply)
}

// touch records that a memory was used.
//
// A touch that matched nothing is an error rather than a quiet success: the
// caller has just used a memory that is not there, and the C answers the same
// way. The reply is empty, so the status is the only place to say it.
func touch(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeTouchRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, touchQuery,
		db2contract.EncodeTouchReply, int64(memoryID))
}

func linkDelete(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	linkID, err := db2contract.DecodeLinkDeleteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The C answers on the statement running, not on a row going, because a
	// link already deleted is the state the caller wanted.
	if _, execErr := store.Exec(ctx, linkDeleteQuery,
		int64(linkID)); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(db2contract.EncodeLinkDeleteReply)
}

// validAt answers whether a memory was in force at an instant.
//
// Two fields, and the first is why: a memory that cannot be found, or a
// comparison that cannot be evaluated, is not the same as one that was out of
// force. The C carries that as invalid_state so an unanswered question cannot
// be read as a "no".
func validAt(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, asOf, err := db2contract.DecodeValidAtRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var inForce bool
	if scanErr := store.QueryRow(ctx, validAtQuery, int64(memoryID), asOf).
		Scan(&inForce); scanErr != nil {
		reply, encodeErr := db2contract.EncodeValidAtReply(
			db2contract.ResultInvalidState, 0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	value := uint32(0)
	if inForce {
		value = 1
	}
	reply, encodeErr := db2contract.EncodeValidAtReply(db2contract.ResultOK, value)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func hasScopeType(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, scopeType, err := db2contract.DecodeHasScopeTypeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, hasScopeTypeQuery,
		db2contract.EncodeHasScopeTypeReply, int64(memoryID), scopeType)
}

func reject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeRejectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return changedRowRequired(ctx, store, rejectQuery,
		db2contract.EncodeRejectReply, int64(memoryID))
}

func updateContent(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, content, err := db2contract.DecodeUpdateContentRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	updated, execErr := store.Exec(ctx, updateContentQuery, int64(memoryID),
		content)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(updated, db2contract.EncodeUpdateContentReply)
}

func decayConfidence(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeDecayConfidenceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return voidWrite(ctx, store, decayConfidenceQuery, "decay_confidence",
		db2contract.EncodeDecayConfidenceReply, int64(memoryID))
}

func workspaceTagInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, workspace, err :=
		db2contract.DecodeWorkspaceTagInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return voidWrite(ctx, store, workspaceTagInsertQuery, "workspace_tag_insert",
		db2contract.EncodeWorkspaceTagInsertReply, int64(memoryID), workspace)
}

func setCognifiedKind(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, kind, err := db2contract.DecodeSetCognifiedKindRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if kind == "" {
		// The C refuses an empty kind before the statement, and its reply has
		// no field to say so -- so the write simply does not happen.
		return emptyReply(db2contract.EncodeSetCognifiedKindReply)
	}
	return voidWrite(ctx, store, setCognifiedKindQuery, "set_cognified_kind",
		db2contract.EncodeSetCognifiedKindReply, int64(memoryID), kind)
}

func setSourceSession(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, sessionID, err :=
		db2contract.DecodeSetSourceSessionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return voidWrite(ctx, store, setSourceSessionQuery, "set_source_session",
		db2contract.EncodeSetSourceSessionReply, int64(memoryID), sessionID)
}

func negationTokensUpdate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, tokens, err :=
		db2contract.DecodeNegationTokensUpdateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return voidWrite(ctx, store, negationTokensUpdateQuery,
		"negation_tokens_update",
		db2contract.EncodeNegationTokensUpdateReply, int64(memoryID), tokens)
}

func recordExists(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	recordID, err := db2contract.DecodeRecordExistsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return existsReply(ctx, store, recordExistsQuery,
		db2contract.EncodeRecordExistsReply, int64(recordID))
}

// changedRowRequired runs a statement whose reply is empty and whose failure
// mode is "nothing matched".
//
// The C returns an error when its update changed no rows, and the adapter turns
// that into an internal status. There is nowhere else to put it: the reply
// carries no fields at all, so a caller that touched a memory which is not
// there learns it from the status or not at all.
func changedRowRequired(ctx context.Context, store Store, query string,
	encode func() ([]byte, error), args ...any) ([]byte, bus.ModuleStatus) {
	changed, execErr := store.Exec(ctx, query, args...)
	if execErr != nil || changed == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return emptyReply(encode)
}

// voidWrite runs a statement whose C backend returns void and whose reply
// carries nothing.
//
// As with dispatchAcknowledgement, the error has nowhere to go in the reply, so
// it is logged rather than dropped. Unlike that one there is not even a flag to
// answer, which is why the status stays OK: the C's adapter cannot fail here
// either, and answering an error would be this implementation inventing a
// signal the contract does not have.
func voidWrite(ctx context.Context, store Store, query, operation string,
	encode func() ([]byte, error), args ...any) ([]byte, bus.ModuleStatus) {
	if _, execErr := store.Exec(ctx, query, args...); execErr != nil {
		logDroppedWrite(operation, execErr)
	}
	return emptyReply(encode)
}

// emptyReply answers a reply with no fields.
func emptyReply(encode func() ([]byte, error)) ([]byte, bus.ModuleStatus) {
	reply, err := encode()
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// infallible adapts an encoder that cannot fail.
//
// A reply with no fields and a fixed length has nothing to fail on, so the
// generator emits those without an error return. The helpers here take the
// fallible shape because most replies have one, and this is the adapter rather
// than a second copy of each helper.
func infallible(encode func() []byte) func() ([]byte, error) {
	return func() ([]byte, error) { return encode(), nil }
}

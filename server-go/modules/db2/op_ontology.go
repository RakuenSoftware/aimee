package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageOntologyEvalStatus,
		db2contract.OperationOntologyEvalStatus, ontologyEvalStatus)
	Register(db2contract.StageOntologyApprove,
		db2contract.OperationOntologyApprove, ontologyApprove)
	Register(db2contract.StageOntologyReject,
		db2contract.OperationOntologyReject, ontologyReject)
	Register(db2contract.StageReleaseCreate,
		db2contract.OperationReleaseCreate, releaseCreate)
}

const ontologyEvalStatusQuery = `SELECT status FROM ontology_evaluations
 WHERE rel_type = $1 LIMIT 1`

// ontologyEvalStatus reads where a proposed relation type stands.
func ontologyEvalStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	relType, err := db2contract.DecodeOntologyEvalStatusRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	normalized := normalizeRelType(relType)
	if normalized == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	status, moduleStatus := readOptionalText(ctx, store, ontologyEvalStatusQuery, normalized)
	if moduleStatus != bus.ModuleStatusOK {
		return nil, moduleStatus
	}
	reply, err := db2contract.EncodeOntologyEvalStatusReply(status)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Two tables move together on every ontology decision: ontology_evaluations
// records the decision, rel_types records what the relation is now allowed to
// do. The evaluation is the one that must exist -- deciding on a relation
// nobody proposed is the caller getting it wrong -- and the rel_types update is
// best-effort, because a seeded relation type may already be in the state the
// decision would put it in.
//
// decided_at is written space-separated with no zone marker, UTC by
// construction. That is the format these tables hold and it is not the ISO-8601
// form the provenance writer uses; the two are not interchangeable.
const (
	ontologyEvaluationApproveQuery = `UPDATE ontology_evaluations
 SET status = 'approved',
     decided_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')
 WHERE rel_type = $1`
	ontologyEvaluationRejectQuery = `UPDATE ontology_evaluations
 SET status = 'rejected',
     decided_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')
 WHERE rel_type = $1`
	relTypeActivateQuery = `UPDATE rel_types SET status = 'active' WHERE rel_type = $1`
	relTypeRejectQuery   = `UPDATE rel_types SET status = 'rejected' WHERE rel_type = $1`
)

var errNoSuchEvaluation = errors.New(
	"db2: no evaluation is open for that relation type")

// ontologyApprove admits a proposed relation type into the ontology.
func ontologyApprove(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	relType, err := db2contract.DecodeOntologyApproveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := decideOntology(ctx, store, relType,
		ontologyEvaluationApproveQuery, relTypeActivateQuery)
	return acknowledgement(txErr == nil, db2contract.EncodeOntologyApproveReply)
}

// ontologyReject refuses a proposed relation type.
func ontologyReject(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	relType, err := db2contract.DecodeOntologyRejectRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	txErr := decideOntology(ctx, store, relType,
		ontologyEvaluationRejectQuery, relTypeRejectQuery)
	return acknowledgement(txErr == nil, db2contract.EncodeOntologyRejectReply)
}

// decideOntology records a decision against both tables, or neither.
//
// The evaluation update must change exactly one row. More than one would mean
// the same relation type has two open evaluations, which the caller has no way
// to disambiguate; none means there is nothing to decide. Either way the
// rel_types change must not stand on its own -- a relation marked active with
// no approved evaluation behind it is a permission granted by nobody.
func decideOntology(ctx context.Context, store Store, relType, evaluation, relTypes string) error {
	normalized := normalizeRelType(relType)
	if normalized == "" {
		return errNoSuchEvaluation
	}
	return store.InTx(ctx, func(tx Store) error {
		decided, err := tx.Exec(ctx, evaluation, normalized)
		if err != nil {
			return err
		}
		if decided != 1 {
			return errNoSuchEvaluation
		}
		// Best-effort in one specific sense: matching no row is fine, because a
		// seeded relation type may already carry this status and refusing a
		// correctly recorded decision over that would be wrong.
		//
		// It does not mean a failing statement is survivable. A statement that
		// errors inside a transaction poisons it, so the commit fails and the
		// whole decision rolls back -- which is also what the C gets, since it
		// ignores the return value but not the aborted transaction underneath.
		_, _ = tx.Exec(ctx, relTypes, normalized)
		return nil
	})
}

const releaseCreateQuery = `INSERT INTO doc_releases (name) VALUES ($1) RETURNING id`

// releaseCreate opens a documentation release.
//
// Every other column takes its default, so a fresh release is 'pending' with no
// promoted or retired timestamp -- kb_release_promote is what moves it. The name
// is unique, so creating one twice is a constraint violation rather than a
// second release, which is what stops a retry from forking the release history.
func releaseCreate(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	name, err := db2contract.DecodeReleaseCreateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id int64
	if scanErr := store.QueryRow(ctx, releaseCreateQuery, name).Scan(&id); scanErr != nil {
		// A duplicate name lands here. Zero says "no release was created",
		// which is the honest answer and the one the C gives.
		reply, encodeErr := db2contract.EncodeReleaseCreateReply(0)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	reply, err := db2contract.EncodeReleaseCreateReply(clampToU64(id))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

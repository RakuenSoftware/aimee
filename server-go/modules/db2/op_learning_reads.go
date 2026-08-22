package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageBanditArmsList,
		db2contract.OperationBanditArmsList, banditArmsList)
	Register(db2contract.StageBanditPromotionGet,
		db2contract.OperationBanditPromotionGet, banditPromotionGet)
	Register(db2contract.StageCalibrationSurfacesWithData,
		db2contract.OperationCalibrationSurfacesWithData, calibrationSurfacesWithData)
	Register(db2contract.StageArtifactTargetSurface,
		db2contract.OperationArtifactTargetSurface, artifactTargetSurface)
	Register(db2contract.StageAuditLatestBefore,
		db2contract.OperationAuditLatestBefore, auditLatestBefore)
	Register(db2contract.StageEvidencePendingList,
		db2contract.OperationEvidencePendingList, evidencePendingList)
}

const banditArmsListQuery = `SELECT DISTINCT arm_id
 FROM bandit_decisions
 WHERE decision_point = $1
 ORDER BY arm_id`

// banditArmsList lists the arms a decision point has ever chosen, as a JSON
// array.
//
// Read from the decisions rather than from a register of arms, so an arm that
// has never been chosen does not appear. That is what the caller wants -- it is
// asking what this decision point actually does -- and it means a newly added
// arm is invisible until it is first tried.
//
// Ordered by name, not by use: this is a set, and a stable order makes two
// reads comparable.
func banditArmsList(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	decisionPoint, err := db2contract.DecodeBanditArmsListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	arms, status := readStringArray(ctx, store, banditArmsListQuery, decisionPoint)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeBanditArmsListReply(arms)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const banditPromotionGetQuery = `SELECT arm_id FROM bandit_promotions
 WHERE decision_point = $1`

// banditPromotionGet reads the arm a decision point has been pinned to.
//
// Empty means the decision point is still exploring, which is the ordinary
// state and not an error. The C treats an empty arm_id the same as no row at
// all, and so does this: a promotion to nothing is not a promotion.
func banditPromotionGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionPoint, err := db2contract.DecodeBanditPromotionGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	arm, status := readOptionalText(ctx, store, banditPromotionGetQuery, decisionPoint)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeBanditPromotionGetReply(arm)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const calibrationSurfacesWithDataQuery = `SELECT COUNT(*) FROM (
 SELECT ae.target_surface, a.kind, ae.scope_kind, ae.scope_id
 FROM audit_events ae
 JOIN artifacts a ON a.id = ae.source_artifact_id
 WHERE ae.verdict <> ''
 GROUP BY ae.target_surface, a.kind, ae.scope_kind, ae.scope_id
 HAVING COUNT(*) >= $1
) AS t`

// calibrationSurfacesWithData counts the surfaces carrying enough judged
// events to calibrate against.
//
// A surface here is the four-way grouping of target surface, artifact kind and
// scope, not the target surface alone: the same surface calibrates separately
// per kind and per scope, because a verdict on one says little about another.
// Only events with a verdict count -- an unjudged event is not evidence.
func calibrationSurfacesWithData(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	minimum, err := db2contract.DecodeCalibrationSurfacesWithDataRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// Zero would make HAVING COUNT(*) >= 0 admit every group, including those
	// with no rows to speak of, so the C floors it at one. The contract may
	// already refuse zero; the floor stays because it is the reason the bound
	// exists and because Op is callable without the wire.
	threshold := int64(minimum)
	if threshold < 1 {
		threshold = 1
	}
	count, status := readOptionalInt(ctx, store, calibrationSurfacesWithDataQuery, threshold)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeCalibrationSurfacesWithDataReply(clampToU32(count))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const artifactTargetSurfaceQuery = `SELECT target_surface FROM artifacts
 WHERE id = $1 LIMIT 1`

// artifactTargetSurface reads which surface an artifact was produced for.
func artifactTargetSurface(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeArtifactTargetSurfaceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	surface, status := readOptionalText(ctx, store, artifactTargetSurfaceQuery, artifactID)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeArtifactTargetSurfaceReply(surface)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const auditLatestBeforeQuery = `SELECT before_snapshot FROM audit_events
 WHERE source_artifact_id = $1
 ORDER BY id DESC LIMIT 1`

// auditLatestBefore reads the state captured before the most recent audited
// change to an artifact.
//
// Ordered by id rather than by a timestamp, so "most recent" means most
// recently written. Two events stamped the same second still order, which a
// timestamp ordering would leave to the planner -- and this is what an undo
// reads, so the wrong one of two is worse than none.
func auditLatestBefore(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, err := db2contract.DecodeAuditLatestBeforeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	snapshot, status := readOptionalText(ctx, store, auditLatestBeforeQuery, artifactID)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeAuditLatestBeforeReply(snapshot)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const evidencePendingListQuery = `SELECT artifact_id, collection FROM evidence_index_ops
 WHERE status = 'pending' ORDER BY artifact_id LIMIT $1`

// evidencePendingList lists the evidence still waiting to be indexed.
//
// Ordered by artifact rather than by age, so a caller draining this sees the
// same artifact's work together. Nothing here reports how long a row has been
// pending, which means a row that never succeeds is indistinguishable from one
// queued a moment ago -- the retry accounting lives in the ops table's own
// attempt count, not in this read.
func evidencePendingList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeEvidencePendingListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.EvidencePendingListMaxRows
	rows, queryErr := store.Query(ctx, evidencePendingListQuery, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.EvidencePendingListRow, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var artifactID, collection *string
		if err := rows.Scan(&artifactID, &collection); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, db2contract.EvidencePendingListRow{
			ArtifactID: text(artifactID),
			Collection: text(collection),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeEvidencePendingListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

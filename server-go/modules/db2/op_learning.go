package db2

import (
	"context"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageAuditEventList,
		db2contract.OperationAuditEventList, auditEventList)
	Register(db2contract.StageDemotionCandidates,
		db2contract.OperationDemotionCandidates, demotionCandidates)
}

const auditEventListSelect = `SELECT id, target_surface, target_id, operator_id, scope_kind,
 scope_id, applied_at, applied_confidence, flagged_for_review
 FROM audit_events WHERE applied_at >= $1`

// auditEventList reads applied artifact decisions in a time window.
//
// The start is required and the backend refuses without one, which is the rule
// that keeps this from becoming a full-table scan: the cheapest way to ask for
// everything is closed deliberately. The end and the scope are optional and
// widen by being empty, so a caller that fails to assemble a scope reads across
// all of them rather than none.
//
// The statement is assembled from the arguments present, as the C one is. The
// alternative -- one statement with an is-empty-or-matches predicate per
// optional argument -- would read the same rows and plan differently, and a
// parity comparison of query plans is not one anybody wants to be making.
func auditEventList(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	since, until, scopeKind, limit, err := db2contract.DecodeAuditEventListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if since == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var statement strings.Builder
	statement.WriteString(auditEventListSelect)
	arguments := []any{since}
	if until != "" {
		arguments = append(arguments, until)
		statement.WriteString(" AND applied_at <= $" + strconv.Itoa(len(arguments)))
	}
	if scopeKind != "" {
		arguments = append(arguments, scopeKind)
		statement.WriteString(" AND scope_kind = $" + strconv.Itoa(len(arguments)))
	}
	bounded := int(limit)
	if bounded > db2contract.AuditEventListMaxRows {
		bounded = db2contract.AuditEventListMaxRows
	}
	arguments = append(arguments, bounded)
	statement.WriteString(" ORDER BY applied_at DESC LIMIT $" + strconv.Itoa(len(arguments)))

	rows, err := store.Query(ctx, statement.String(), arguments...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.AuditEventListRow, 0, bounded)
	for rows.Next() {
		var (
			id               string
			targetSurface    string
			targetID         string
			operatorID       string
			eventScopeKind   string
			scopeID          string
			appliedAt        string
			confidence       float64
			flaggedForReview bool
		)
		if err := rows.Scan(&id, &targetSurface, &targetID, &operatorID, &eventScopeKind,
			&scopeID, &appliedAt, &confidence, &flaggedForReview); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		flagged := uint32(0)
		if flaggedForReview {
			flagged = 1
		}
		found = append(found, db2contract.AuditEventListRow{
			EventID:           id,
			TargetSurface:     targetSurface,
			TargetID:          targetID,
			OperatorID:        operatorID,
			EventScopeKind:    eventScopeKind,
			ScopeID:           scopeID,
			AppliedAt:         appliedAt,
			AppliedConfidence: confidence,
			FlaggedForReview:  flagged,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeAuditEventListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const demotionCandidatesQuery = `SELECT scope_id, COUNT(*) AS n
 FROM artifacts
 WHERE kind = 'retrieval_attribution'
 GROUP BY scope_id
 HAVING COUNT(*) >= $1
 LIMIT $2`

// demotionCandidates finds what has been retrieved often enough to be worth
// demoting.
//
// The scope identifier is text in the table and a number on the wire, because
// the thing being demoted is a row. A scope whose identifier is not numeric is
// skipped, so the count returned is smaller than the number of groups without
// saying so -- the C implementation does the same through atoll, which answers
// zero for anything unparseable and is why this checks rather than converts.
func demotionCandidates(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	minimum, err := db2contract.DecodeDemotionCandidatesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The C implementation floors the minimum at one: a HAVING of zero would
	// return every group, which is not a candidate list.
	floor := int(minimum)
	if floor < 1 {
		floor = 1
	}

	rows, err := store.Query(ctx, demotionCandidatesQuery, floor,
		db2contract.DemotionCandidatesMaxRows)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.DemotionCandidatesRow, 0,
		db2contract.DemotionCandidatesMaxRows)
	for rows.Next() {
		var (
			scopeID string
			count   int64
		)
		if err := rows.Scan(&scopeID, &count); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		if scopeID == "" {
			continue
		}
		rowID, parseErr := strconv.ParseInt(scopeID, 10, 64)
		if parseErr != nil {
			// atoll answers zero here, which would name row zero. Skipping is
			// what the C loop means rather than what it does, and naming a row
			// that does not exist is worse than one fewer candidate.
			continue
		}
		found = append(found, db2contract.DemotionCandidatesRow{
			CandidateRowID:   clampToU64(rowID),
			AttributionCount: clampToU32(count),
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeDemotionCandidatesReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

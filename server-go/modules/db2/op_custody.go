package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEnrollmentList,
		db2contract.OperationEnrollmentList, enrollmentList)
}

// Column order is the C implementation's ENROLL_COLS. Note that `legacy` comes
// before `authority_id` there and after it in the reply schema, so the scan
// order and the row construction differ deliberately -- following the SELECT
// rather than reordering it keeps the statement identical to the C one.
const enrollmentListQuery = `SELECT id, scope, fingerprint, serial, state, issued_at,
 last_seen_at, expires_at, revoked_at, legacy, authority_id
 FROM kb_enrollments ORDER BY id DESC LIMIT $1`

// enrollmentList lists enrolled certificates, newest first.
//
// Revoked rows are listed alongside live ones with their revocation stamp: the
// question an operator asks is which certificates this install has seen, and an
// answer omitting the revoked ones invites re-enrolling something deliberately
// withdrawn.
func enrollmentList(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus) {
	limit, err := db2contract.DecodeEnrollmentListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The C implementation clamps a limit above the caller's array to the array,
	// so the ceiling is the reply's either way.
	bounded := int(limit)
	if bounded > db2contract.EnrollmentListMaxRows {
		bounded = db2contract.EnrollmentListMaxRows
	}

	rows, err := store.Query(ctx, enrollmentListQuery, bounded)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]db2contract.EnrollmentListRow, 0, bounded)
	for rows.Next() {
		var (
			id          int64
			scope       string
			fingerprint string
			serial      string
			state       string
			issuedAt    string
			lastSeenAt  string
			expiresAt   string
			revokedAt   string
			legacy      int32
			authorityID string
		)
		if err := rows.Scan(&id, &scope, &fingerprint, &serial, &state, &issuedAt,
			&lastSeenAt, &expiresAt, &revokedAt, &legacy, &authorityID); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		legacyFlag := uint32(0)
		if legacy != 0 {
			legacyFlag = 1
		}
		found = append(found, db2contract.EnrollmentListRow{
			EnrollmentID:    clampToU64(id),
			EnrollmentScope: scope,
			CertFingerprint: fingerprint,
			CertSerialNorm:  serial,
			EnrollmentState: state,
			IssuedAt:        issuedAt,
			LastSeenAt:      lastSeenAt,
			ExpiresAt:       expiresAt,
			RevokedAt:       revokedAt,
			AuthorityID:     authorityID,
			LegacyRow:       legacyFlag,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, err := db2contract.EncodeEnrollmentListReply(found)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageKBDocRead,
		db2contract.OperationKBDocRead, kbDocRead)
	Register(db2contract.StageKBDocListReview,
		db2contract.OperationKBDocListReview, kbDocListReview)
	Register(db2contract.StageKBDocAssetsList,
		db2contract.OperationKBDocAssetsList, kbDocAssetsList)
	Register(db2contract.StageKBAsyncJobGet,
		db2contract.OperationKBAsyncJobGet, kbAsyncJobGet)
	Register(db2contract.StageEnrollmentRevoke,
		db2contract.OperationEnrollmentRevoke, enrollmentRevoke)
	Register(db2contract.StageTypedFactRecall,
		db2contract.OperationTypedFactRecall, typedFactRecall)
}

// The eleven columns both document reads return.
const docColumns = `SELECT id, content_hash, filename, scope, converter,
 converter_version, state, review_needed, review_reason, created_at, updated_at
 FROM docs`

const (
	kbDocReadQuery = docColumns + ` WHERE id = $1`
	// Keyset paging rather than an offset: the cursor is the last identifier
	// seen, so a document reviewed and cleared between pages does not shift the
	// window and hide the next one. Ascending, because the cursor only moves
	// forward.
	kbDocListReviewQuery = docColumns +
		` WHERE state = 'staged' AND review_needed = true AND id > $2
 ORDER BY id ASC LIMIT $1`
)

// docScanner is the narrow slice of a row reader both document reads use.
type docScanner interface {
	Scan(dest ...any) error
}

func scanDoc(row docScanner) (db2contract.KBDocListReviewRow, error) {
	var id int64
	var reviewNeeded bool
	var hash, filename, scope, converter, version string
	var state, reason, createdAt, updatedAt string
	if err := row.Scan(&id, &hash, &filename, &scope, &converter, &version,
		&state, &reviewNeeded, &reason, &createdAt, &updatedAt); err != nil {
		return db2contract.KBDocListReviewRow{}, err
	}
	return db2contract.KBDocListReviewRow{
		DocID:            clampToU64(id),
		ContentHash:      hash,
		DocFilename:      filename,
		DocScope:         scope,
		Converter:        converter,
		ConverterVersion: version,
		DocState:         state,
		ReviewNeeded:     boolToU32(reviewNeeded),
		ReviewReason:     reason,
		DocCreatedAt:     createdAt,
		DocUpdatedAt:     updatedAt,
	}, nil
}

// kbDocRead answers one document's metadata.
//
// The converter and its version travel together because a document converted
// by an older version may need re-converting: the pair is what a migration
// compares, and either alone says nothing.
func kbDocRead(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	docID, err := db2contract.DecodeKBDocReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	row, scanErr := scanDoc(store.QueryRow(ctx, kbDocReadQuery, int64(docID)))
	found := uint32(1)
	if scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, row = 0, db2contract.KBDocListReviewRow{}
	}
	reply, encodeErr := db2contract.EncodeKBDocReadReply(found, row.ContentHash,
		row.DocFilename, row.DocScope, row.Converter, row.ConverterVersion,
		row.DocState, row.ReviewNeeded, row.ReviewReason, row.DocCreatedAt,
		row.DocUpdatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// kbDocListReview lists documents waiting for a person to look at them.
//
// Staged and flagged, both: a document can be flagged and already published,
// which is a different queue -- this one is what has not been let through yet.
func kbDocListReview(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	rowLimit, cursorID, err :=
		db2contract.DecodeKBDocListReviewRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.KBDocListReviewMaxRows
	rows, queryErr := store.Query(ctx, kbDocListReviewQuery,
		int64(pairLimit(rowLimit, ceiling)), int64(cursorID))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	waiting := make([]db2contract.KBDocListReviewRow, 0, 16)
	for rows.Next() && len(waiting) < ceiling {
		row, scanErr := scanDoc(rows)
		if scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		waiting = append(waiting, row)
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeKBDocListReviewReply(waiting)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Gated through the authoritative document row rather than read from the asset
// table directly, and the C's note says why: the join is what decides whether
// the caller may see the asset at all.
//
// Four conditions do that gating. The document must be a PDF, must not be
// quarantined, must be in its project's current generation, and the asset's
// generation must match the document's -- so an asset left behind by a
// superseded ingest is not reachable through a current document.
//
// blob_ref is deliberately not selected. The reply carries where an asset sits
// on the page and what it is, and never how to fetch its bytes.
const kbDocAssetsListQuery = `SELECT DISTINCT a.id, a.page_no, a.x0, a.y0, a.x1,
 a.y1, a.kind, a.caption, a.content_type, a.sensitivity_class
 FROM kb_doc_assets a
 JOIN kb_documents d ON d.file_path = a.document_key
 WHERE d.project = $1 AND d.doc_kind = 'pdf'
   AND d.quarantine_state <> 'pending'
   AND a.project = $1
   AND a.generation = d.generation
   AND d.generation = (SELECT current_generation FROM projects
     WHERE name = d.project AND lifecycle_state = 'current')
   AND d.file_path = $2
 ORDER BY a.page_no, a.id
 LIMIT $3`

// kbDocAssetsList lists the figures and tables a document carries.
//
// DISTINCT because a document is many chunks and the join is on the file path,
// so one asset matches every chunk of its document. Ordered by page and then
// identifier, which is reading order for a document.
func kbDocAssetsList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, documentKey, err :=
		db2contract.DecodeKBDocAssetsListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.KBDocAssetsListMaxRows
	rows, queryErr := store.Query(ctx, kbDocAssetsListQuery,
		project, documentKey, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	assets := make([]db2contract.KBDocAssetsListRow, 0, 16)
	for rows.Next() {
		var id, page int64
		var x0, y0, x1, y1 float64
		var kind, caption, contentType, sensitivity string
		if scanErr := rows.Scan(&id, &page, &x0, &y0, &x1, &y1, &kind, &caption,
			&contentType, &sensitivity); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		assets = append(assets, db2contract.KBDocAssetsListRow{
			AssetID:          clampToU64(id),
			PageNo:           clampToU32(page),
			X0:               x0,
			Y0:               y0,
			X1:               x1,
			Y1:               y1,
			AssetKind:        kind,
			AssetCaption:     caption,
			ContentType:      contentType,
			SensitivityClass: sensitivity,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeKBDocAssetsListReply(assets)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const kbAsyncJobGetQuery = `SELECT document_id, kind, project, status, attempts,
 last_error, claimed_by, claimed_at, created_at, updated_at
 FROM kb_async_jobs WHERE id = $1`

// kbAsyncJobGet answers what an asynchronous job is doing.
//
// claimed_by and claimed_at are what make a stuck job visible: a job running
// for an hour with a worker name on it was claimed by something that has since
// died, and nothing else in the row distinguishes that from work in progress.
func kbAsyncJobGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	jobID, err := db2contract.DecodeKBAsyncJobGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var documentID, attempts int64
	var kind, project, status, lastError string
	var claimedBy, claimedAt, createdAt, updatedAt string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, kbAsyncJobGetQuery, int64(jobID)).
		Scan(&documentID, &kind, &project, &status, &attempts, &lastError,
			&claimedBy, &claimedAt, &createdAt, &updatedAt); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, documentID, attempts = 0, 0, 0
		kind, project, status, lastError = "", "", "", ""
		claimedBy, claimedAt, createdAt, updatedAt = "", "", "", ""
	}
	reply, encodeErr := db2contract.EncodeKBAsyncJobGetReply(found,
		clampToU64(documentID), kind, project, status, clampToU32(attempts),
		lastError, claimedBy, claimedAt, createdAt, updatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The revocation stamp is set only if it is not already set, which is what
// makes revoking twice safe: the first revocation is when it happened, and a
// second call must not move it forward.
//
// RETURNING hands back the row as it now stands, so a caller sees the
// revocation it caused rather than the one it asked for.
const enrollmentRevokeQuery = `UPDATE kb_enrollments
 SET state = 'revoked',
     revoked_at = COALESCE(NULLIF(revoked_at, ''), pg_now_text())
 WHERE id = $1
 RETURNING scope, fingerprint, cert_serial_norm, state, issued_at,
 last_seen_at, expires_at, revoked_at, authority_id, legacy`

// enrollmentRevoke withdraws a certificate's enrolment.
//
// Revoking one that is already revoked is not an error and not a no-op either:
// the row comes back with its original revocation time, which is the answer a
// caller retrying needs.
//
// The C returns the plain serial column here while the resolve path matches on
// the normalized one. The reply's field is named for the normalized value, so
// that is what this selects: a caller using this reply to look the enrolment up
// again needs the value the lookup matches on.
//
// The two agree for anything the enrolment write created -- it binds the
// normalized serial to both columns. They differ only for a legacy row
// backfilled before the normalized column existed, where the plain serial has a
// value and the normalized one is empty. Answering empty there is the honest
// reply: that row genuinely has no revocation key yet.
func enrollmentRevoke(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	enrollmentID, err := db2contract.DecodeEnrollmentRevokeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var legacy int64
	var scope, fingerprint, serialNorm, state string
	var issuedAt, lastSeenAt, expiresAt, revokedAt, authorityID string
	revoked := uint32(1)
	if scanErr := store.QueryRow(ctx, enrollmentRevokeQuery, int64(enrollmentID)).
		Scan(&scope, &fingerprint, &serialNorm, &state, &issuedAt, &lastSeenAt,
			&expiresAt, &revokedAt, &authorityID, &legacy); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		revoked, legacy = 0, 0
		scope, fingerprint, serialNorm, state = "", "", "", ""
		issuedAt, lastSeenAt, expiresAt, revokedAt, authorityID = "", "", "", "", ""
	}
	reply, encodeErr := db2contract.EncodeEnrollmentRevokeReply(revoked, scope,
		fingerprint, serialNorm, state, issuedAt, lastSeenAt, expiresAt,
		revokedAt, authorityID, clampToU32(legacy))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Only active facts. A superseded fact is still a row -- the table keeps its
// history -- and recalling it would answer with something the graph has since
// replaced.
//
// The ordering differs with the filter, which is the C's and is right: asked
// for one relation, the identifier alone orders it; asked for all of them,
// grouping by relation first is what makes the answer readable.
const (
	typedFactRecallQuery = `SELECT id, subject, subject_kind, relation, object,
 object_kind, confidence, source, asserted_at
 FROM typed_facts
 WHERE subject = $2 AND active = 1 AND ($3 = '' OR relation = $3)
 ORDER BY CASE WHEN $3 = '' THEN relation ELSE '' END, id
 LIMIT $1`
)

// typedFactRecall answers what is known about a subject.
func typedFactRecall(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	subject, relationFilter, limit, err :=
		db2contract.DecodeTypedFactRecallRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.TypedFactRecallMaxRows
	rows, queryErr := store.Query(ctx, typedFactRecallQuery,
		int64(pairLimit(limit, ceiling)), subject, relationFilter)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	facts := make([]db2contract.TypedFactRecallRow, 0, 16)
	for rows.Next() {
		var id, confidence int64
		var subjectValue, subjectKind, relation string
		var object, objectKind, source, assertedAt string
		if scanErr := rows.Scan(&id, &subjectValue, &subjectKind, &relation,
			&object, &objectKind, &confidence, &source,
			&assertedAt); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		facts = append(facts, db2contract.TypedFactRecallRow{
			FactID:         clampToU64(id),
			FactConfidence: clampToU32(confidence),
			Subject:        subjectValue,
			SubjectKind:    subjectKind,
			FactRelation:   relation,
			Object:         object,
			ObjectKind:     objectKind,
			FactSource:     source,
			AssertedAt:     assertedAt,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTypedFactRecallReply(facts)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

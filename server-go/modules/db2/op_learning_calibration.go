package db2

import (
	"context"
	"crypto/rand"
	"fmt"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageRulesInsert,
		db2contract.OperationRulesInsert, rulesInsert)
	Register(db2contract.StageRulesReinforceDirective,
		db2contract.OperationRulesReinforceDirective, rulesReinforceDirective)
	Register(db2contract.StageDemotionProfileWrite,
		db2contract.OperationDemotionProfileWrite, demotionProfileWrite)
	Register(db2contract.StageRetrievalAttributionWrite,
		db2contract.OperationRetrievalAttributionWrite, retrievalAttributionWrite)
	Register(db2contract.StageCalibrationSurfaceList,
		db2contract.OperationCalibrationSurfaceList, calibrationSurfaceList)
}

const rulesInsertQuery = `INSERT INTO rules
 (polarity, title, description, weight, created_at, updated_at)
 VALUES ($1, $2, $3, $4, pg_now_text(), pg_now_text())`

// rulesInsert records a new rule.
//
// An empty polarity becomes "positive". That default is the C's and it is
// reachable here, unlike most of the ones the adapter's buffers hide: the
// envelope allows an empty polarity, and the column has no default of its own
// to fall back on. A rule stored with an empty polarity would render without
// the symbol that says whether it is a do or a do-not.
func rulesInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	polarity, title, description, weight, err :=
		db2contract.DecodeRulesInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if polarity == "" {
		polarity = "positive"
	}
	_, execErr := store.Exec(ctx, rulesInsertQuery,
		polarity, title, description, int64(weight))
	return acknowledgement(execErr == nil, db2contract.EncodeRulesInsertReply)
}

// Two statements, because the weight is optional and leaving it out is not the
// same as setting it to zero -- a rule reinforced without a new weight keeps
// the one it earned. Both stamp updated_at and last_reinforced_at, which are
// different questions: when the row last changed, and when someone last said
// the rule still holds.
const (
	rulesReinforceQuery = `UPDATE rules
 SET directive_type = $1, updated_at = pg_now_text(),
 last_reinforced_at = pg_now_text()
 WHERE id = $2`
	rulesReinforceWithWeightQuery = `UPDATE rules
 SET directive_type = $1, weight = $2, updated_at = pg_now_text(),
 last_reinforced_at = pg_now_text()
 WHERE id = $3`
)

// rulesReinforceDirective records that a rule still holds, optionally at a new
// weight.
//
// The C invalidates an in-process rules cache after the write. There is no such
// cache here, and there cannot be one that helps: the cache lives beside the
// reader, and a Go module answering a request has no way to reach the C
// reader's copy. Whoever owns that cache at cutover has to invalidate it from
// the outside or stop caching.
func rulesReinforceDirective(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	ruleID, directiveType, setWeight, ruleWeight, err :=
		db2contract.DecodeRulesReinforceDirectiveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var execErr error
	if setWeight != 0 {
		_, execErr = store.Exec(ctx, rulesReinforceWithWeightQuery,
			directiveType, int64(ruleWeight), int64(ruleID))
	} else {
		_, execErr = store.Exec(ctx, rulesReinforceQuery, directiveType, int64(ruleID))
	}
	return acknowledgement(execErr == nil,
		db2contract.EncodeRulesReinforceDirectiveReply)
}

// One insert rather than the C's write-then-stamp.
//
// The C reaches these tables through a generic artifact writer that has no
// parameter for target_surface or committed_at, so it inserts and then updates
// the row it just wrote. Writing the statement directly means those columns can
// be set at insert time -- which removes a window in which the artifact exists
// but demotion_profile_read cannot find it, because that read filters on
// target_surface.
//
// The payload is cast explicitly. The column is JSONB and pgx sends the
// parameter as text, so without the cast PostgreSQL refuses the insert; libpq
// left the type unspecified and let the server infer it, which is why the C
// never needed one.
const demotionProfileWriteQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, committed_at, payload)
 VALUES ($1, 'demotion_profile', 'committed', $2, $3, '', 1.0,
  1, '', '', '', $4, pg_now_text(), pg_now_text(), $5::jsonb)
 ON CONFLICT (id) DO NOTHING`

// demotionProfileWrite stores a demotion profile and answers its identifier.
//
// Committed on arrival, unlike most artifacts: a profile is computed rather
// than proposed, so there is nothing for a reviewer to accept. The C's artifact
// writer emits MDL features for anything committed, but only for artifacts of
// kind "synthesis" -- so that branch cannot fire for this kind or for the
// attribution below, and neither carries it.
//
// The scope kind is stored as it arrived, empty included.
//
// The C defaults only a NULL scope kind to "global", and nothing on this wire
// can send NULL, so a profile written with an empty scope kind is stored with
// an empty one -- and the global fallback in demotion_profile_read therefore
// cannot be reached by anything written through this operation. That is a real
// gap, and it is left as it is on purpose: reading an empty scope kind as
// "global" would turn a write whose scope the caller failed to specify into a
// default that applies to every scope there is. Widening a request nobody made
// is worse than storing a profile that only an exact-scope read will find.
func demotionProfileWrite(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryClass, scopeKind, scopeID, payload, err :=
		db2contract.DecodeDemotionProfileWriteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	artifactID, idErr := newArtifactID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	if _, execErr := store.Exec(ctx, demotionProfileWriteQuery,
		artifactID, scopeKind, scopeID, memoryClass, payload); execErr != nil {
		// An empty identifier is the answer for a profile that was not
		// written: no artifact ever carries one.
		artifactID = ""
	}
	reply, encodeErr := db2contract.EncodeDemotionProfileWriteReply(artifactID)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Proposed rather than committed: an attribution is an observation about a
// retrieval, and what it is worth is decided later.
//
// model_version carries the retrieval event identifier, which is how the
// attribution is joined back to the event that produced it, and scope_id
// carries the surfaced row as text so a lookup by row is an index hit. Both are
// set in the insert here; the C stamps model_version afterwards and tolerates
// the stamp failing, which leaves an attribution nothing can join to.
const retrievalAttributionWriteQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 VALUES ($1, 'retrieval_attribution', 'proposed', 'memory', $2, '', 1.0,
  1, '', $3, '', '', pg_now_text(), $4::jsonb)
 ON CONFLICT (id) DO NOTHING`

// retrievalAttributionWrite records what a surfaced row turned out to be worth.
func retrievalAttributionWrite(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	eventID, surfacedRowID, verdict, weight, err :=
		db2contract.DecodeRetrievalAttributionWriteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	artifactID, idErr := newArtifactID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	// The payload is built here rather than taken from the caller, so the four
	// fields are always spelled the same way for whoever reads them back. The
	// weight keeps six decimal places, as the C's format string does.
	payload := fmt.Sprintf(
		`{"retrieval_event_id":%s,"surfaced_row_id":%d,"verdict":%s,"weight":%.6f}`,
		strconv.Quote(eventID), surfacedRowID, strconv.Quote(verdict), weight)
	_, execErr := store.Exec(ctx, retrievalAttributionWriteQuery,
		artifactID, strconv.FormatUint(surfacedRowID, 10), eventID, payload)
	return acknowledgement(execErr == nil,
		db2contract.EncodeRetrievalAttributionWriteReply)
}

// newArtifactID mints the identifier an artifact is written under.
//
// Sixteen random bytes in the shape of a UUID, which is what the C emits. It is
// not a UUID -- no version or variant bits are set -- and the column is TEXT, so
// nothing downstream parses it as one. The shape is kept because artifact
// identifiers written by both sides have to look alike while both sides exist.
func newArtifactID() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x-%x-%x-%x-%x",
		raw[0:4], raw[4:6], raw[6:8], raw[8:10], raw[10:16]), nil
}

// The COALESCEs the C wraps scope_kind and scope_id in are gone: both columns
// are declared NOT NULL with an empty-string default, so the calls could never
// answer anything but the column. A COALESCE over a column that cannot be NULL
// says the schema allows something it does not.
//
// COUNT(*) is repeated in the HAVING rather than naming the alias, because
// PostgreSQL evaluates HAVING before the select list and rejects an alias
// there. ORDER BY is the opposite -- it runs after, so the alias is fine, and
// this statement uses both spellings for that reason.
const calibrationSurfaceListQuery = `SELECT ae.target_surface, a.kind,
 ae.scope_kind, ae.scope_id, COUNT(*) AS n
 FROM audit_events ae
 JOIN artifacts a ON a.id = ae.source_artifact_id
 WHERE ae.verdict <> ''
 GROUP BY ae.target_surface, a.kind, ae.scope_kind, ae.scope_id
 HAVING COUNT(*) >= $1
 ORDER BY n DESC, ae.target_surface, a.kind
 LIMIT $2`

// calibrationSurfaceList answers which surfaces have been reviewed often enough
// to calibrate against.
//
// Only events carrying a verdict count. An audit event with no verdict is one
// nobody has judged yet, and counting it would let a surface reach the
// threshold on reviews that never happened.
//
// A floor of one on the minimum, as the C has: zero would admit every group,
// which is the opposite of what a caller asking for a threshold wants.
func calibrationSurfaceList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	minRows, err := db2contract.DecodeCalibrationSurfaceListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if minRows < 1 {
		minRows = 1
	}
	ceiling := db2contract.CalibrationSurfaceListMaxRows
	rows, queryErr := store.Query(ctx, calibrationSurfaceListQuery,
		int64(minRows), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	surfaces := make([]db2contract.CalibrationSurfaceListRow, 0, 8)
	for rows.Next() && len(surfaces) < ceiling {
		var surface, kind, scopeKind, scopeID string
		var seen int64
		if scanErr := rows.Scan(
			&surface, &kind, &scopeKind, &scopeID, &seen); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		surfaces = append(surfaces, db2contract.CalibrationSurfaceListRow{
			TargetSurface: surface,
			ArtifactKind:  kind,
			ScopeKind:     scopeKind,
			ScopeID:       scopeID,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCalibrationSurfaceListReply(surfaces)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

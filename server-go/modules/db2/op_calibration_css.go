package db2

import (
	"context"
	"sort"
	"strings"
	"unicode"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageArtifactWriteEx,
		db2contract.OperationArtifactWriteEx, artifactWriteEx)
	Register(db2contract.StageAuditEventWrite,
		db2contract.OperationAuditEventWrite, auditEventWrite)
	Register(db2contract.StageCalibrationAuditStats,
		db2contract.OperationCalibrationAuditStats, calibrationAuditStats)
	Register(db2contract.StageCssTokenCandidates,
		db2contract.OperationCssTokenCandidates, cssTokenCandidates)
}

// The same insert as artifact_write with the attempt count taken from the
// caller rather than fixed at one.
//
// That is the whole difference, and it exists for a retry: a caller re-writing
// an artifact after a failed attempt records which attempt this is, so a
// backoff policy has something to read.
const artifactWriteExQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, '', '', '', '', pg_now_text(), $9::jsonb)
 ON CONFLICT (id) DO NOTHING`

// artifactWriteEx stores an artifact and records which attempt produced it.
//
// As with artifact_write, the C's MDL feature emission for committed synthesis
// artifacts is not carried: it is a second operation's worth of work triggered
// by a write, with no catalogue entry of its own.
func artifactWriteEx(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, kind, state, scopeKind, scopeID, operatorID, confidence,
		attemptCount, payload, err :=
		db2contract.DecodeArtifactWriteExRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if state == "" {
		state = "proposed"
	}
	if scopeKind == "" {
		scopeKind = "user"
	}
	attempts := int64(attemptCount)
	if attempts < 1 {
		// An artifact exists because something produced it, so the first
		// attempt is one rather than zero. The C's writer hard-codes one and
		// this variant takes the count, which makes zero expressible and
		// meaningless.
		attempts = 1
	}
	_, execErr := store.Exec(ctx, artifactWriteExQuery, artifactID, kind, state,
		scopeKind, scopeID, operatorID, confidence, attempts, payload)
	return acknowledgement(execErr == nil, db2contract.EncodeArtifactWriteExReply)
}

// Both snapshots are NULLIF-ed before the cast, for the reason the rejection
// write ported earlier carries: the columns are JSONB, an absent snapshot
// arrives as an empty string, and an empty string is not valid JSON -- so
// casting it directly fails the insert.
const auditEventWriteQuery = `INSERT INTO audit_events
 (id, source_artifact_id, target_surface, target_id, operator_id,
  scope_kind, scope_id, applied_at, applied_confidence, flagged_for_review,
  before_snapshot, after_snapshot)
 VALUES ($1, $2, $3, $4, $5, $6, $7, pg_now_text(), $8, $9,
  NULLIF($10, '')::jsonb, NULLIF($11, '')::jsonb)
 ON CONFLICT (id) DO NOTHING`

// auditEventWrite records that something was applied, and what it looked like
// before and after.
//
// The identifier is the caller's, so writing the same event twice records it
// once -- an audit trail that double-counted a retry would misstate how often
// something happened.
//
// An empty scope kind becomes "user", which is the C's default. It is the
// narrowest of the scopes, so an event whose scope nobody set is attributed no
// more widely than it has to be.
func auditEventWrite(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	auditID, sourceArtifactID, targetSurface, targetID, operatorID,
		scopeKind, scopeID, confidence, flagged, before, after, err :=
		db2contract.DecodeAuditEventWriteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if scopeKind == "" {
		scopeKind = "user"
	}
	_, execErr := store.Exec(ctx, auditEventWriteQuery, auditID,
		sourceArtifactID, targetSurface, targetID, operatorID, scopeKind,
		scopeID, confidence, flagged != 0, before, after)
	return acknowledgement(execErr == nil, db2contract.EncodeAuditEventWriteReply)
}

// The confidence is bucketed in SQL, and the two edge cases are why the CASE is
// there rather than a bare multiply: a confidence of exactly one would land in
// a bucket past the last, and a negative one below the first. Both are clamped
// into range rather than dropped, because a judgement with an out-of-range
// confidence still happened.
//
// The window is a subquery when the caller asks for one, so the recent rows are
// chosen before they are bucketed rather than after -- bucketing first and
// limiting after would take a slice of the buckets rather than of the
// judgements.
const calibrationAuditStatsQuery = `WITH recent AS (
   SELECT ae.applied_confidence, ae.verdict
   FROM audit_events ae
   JOIN artifacts a ON a.id = ae.source_artifact_id
   WHERE a.kind = $1 AND ae.target_surface = $2 AND ae.verdict <> ''
     AND ($3 = '' OR ae.scope_kind = $3)
     AND ($3 = '' OR $4 = '' OR ae.scope_id = $4)
   ORDER BY ae.applied_at DESC
   LIMIT $6
 )
 SELECT CASE
     WHEN applied_confidence >= 1.0 THEN $5 - 1
     WHEN applied_confidence < 0.0 THEN 0
     ELSE CAST(applied_confidence * $5 AS INT)
   END AS bucket,
   SUM(CASE WHEN verdict = 'accepted' THEN 1 ELSE 0 END),
   SUM(CASE WHEN verdict = 'rejected' THEN 1 ELSE 0 END),
   COUNT(*)
 FROM recent GROUP BY bucket ORDER BY 1`

// calibrationAuditStatsDefaultWindow is what the C reads when the caller names
// no window: large enough to be every judgement in practice, bounded so the
// statement never scans without a limit.
const calibrationAuditStatsDefaultWindow = 10000

// calibrationAuditStats answers how well a surface's confidence has matched its
// outcomes, bucket by bucket.
//
// Every bucket comes back, including the empty ones. A caller plotting
// calibration needs the gaps: a surface that has never been confident is a
// different picture from one that is confident and wrong, and a reply
// containing only the buckets with rows cannot tell them apart.
//
// alpha and beta are the accepted and rejected counts, named for the Beta
// distribution a caller fits to them. They are counts rather than a fitted
// posterior because the fitting is the caller's, and a prior baked in here
// would be one nobody chose.
func calibrationAuditStats(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetSurface, artifactKind, scopeKind, scopeID, windowRows, err :=
		db2contract.DecodeCalibrationAuditStatsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	buckets := db2contract.CalibrationAuditStatsMaxRows
	window := int64(windowRows)
	if window <= 0 {
		window = calibrationAuditStatsDefaultWindow
	}

	stats := make([]db2contract.CalibrationAuditStatsRow, buckets)
	for index := range stats {
		stats[index] = db2contract.CalibrationAuditStatsRow{
			RangeLo: float64(index) / float64(buckets),
			RangeHi: float64(index+1) / float64(buckets),
		}
	}

	rows, queryErr := store.Query(ctx, calibrationAuditStatsQuery, artifactKind,
		targetSurface, scopeKind, scopeID, int64(buckets), window)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	for rows.Next() {
		var bucket, accepted, rejected, total int64
		if scanErr := rows.Scan(&bucket, &accepted, &rejected,
			&total); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		// A bucket outside the range is dropped rather than clamped: the CASE
		// above already put every row in range, so one arriving out of it means
		// the statement and this loop disagree, and guessing would hide that.
		if bucket < 0 || bucket >= int64(buckets) {
			continue
		}
		stats[bucket].Alpha = float64(accepted)
		stats[bucket].Beta = float64(rejected)
		stats[bucket].SampleN = clampToU32(total)
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCalibrationAuditStatsReply(stats)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Every declaration value in the current generation, counted in Go.
//
// The counting cannot move into SQL: what is counted is literals inside a
// declaration value, and finding them means scanning the text for colour and
// length forms. A value like "1px solid #fff" contributes two.
const cssTokenCandidatesQuery = `SELECT d.value FROM css_declarations d
 JOIN css_rules c ON c.id = d.rule_id
 JOIN files f ON f.id = c.file_id
 JOIN projects p ON p.id = f.project_id
 WHERE d.value <> '' AND ($1 = '' OR p.name = $1)
 AND p.lifecycle_state = 'current'
 AND f.generation = p.current_generation`

// cssTokenMinimumCount is the floor the C applies whatever the caller asks for.
// A literal used once is not a candidate for a design token; it is a one-off.
const cssTokenMinimumCount = 2

// cssToken is one literal and what kind it is.
type cssToken struct {
	value string
	kind  string
}

// hexLiteralLengths are the digit counts a colour may have: three and six are
// the familiar forms, four and eight the same with an alpha channel.
func hexLiteralLength(count int) bool {
	return count == 3 || count == 4 || count == 6 || count == 8
}

// scanCSSLiterals finds the colour and length literals in one declaration
// value.
//
// Colours are hex, or one of the four functional forms with their interior
// whitespace removed so "rgb(0, 0, 0)" and "rgb(0,0,0)" count as one literal.
// Lengths are a number followed immediately by px, rem or em -- and zero is
// excluded, because a zero length needs no token and is written without a unit
// as often as with one.
func scanCSSLiterals(value string, counts map[cssToken]int) {
	lowered := strings.ToLower(value)
	for index := 0; index < len(lowered); {
		switch {
		case lowered[index] == '#':
			digits := index + 1
			for digits < len(lowered) && isHexDigit(lowered[digits]) {
				digits++
			}
			if hexLiteralLength(digits - index - 1) {
				counts[cssToken{lowered[index:digits], "color"}]++
			}
			index = digits
		case colourFunctionAt(lowered, index):
			closing := strings.IndexByte(lowered[index:], ')')
			if closing < 0 {
				index++
				continue
			}
			var literal strings.Builder
			for _, r := range lowered[index : index+closing+1] {
				if !unicode.IsSpace(r) {
					literal.WriteRune(r)
				}
			}
			counts[cssToken{literal.String(), "color"}]++
			index += closing + 1
		case lowered[index] >= '0' && lowered[index] <= '9' &&
			(index == 0 || !isWordByte(lowered[index-1]) && lowered[index-1] != '.'):
			number := index
			for index < len(lowered) &&
				(lowered[index] >= '0' && lowered[index] <= '9' || lowered[index] == '.') {
				index++
			}
			unit := cssLengthUnitAt(lowered, index)
			if unit == 0 {
				continue
			}
			literal := lowered[number : index+unit]
			index += unit
			if strings.Trim(lowered[number:index-unit], "0.") != "" {
				counts[cssToken{literal, "length"}]++
			}
		default:
			index++
		}
	}
}

func isHexDigit(b byte) bool {
	return b >= '0' && b <= '9' || b >= 'a' && b <= 'f'
}

func colourFunctionAt(value string, index int) bool {
	for _, prefix := range []string{"rgba(", "hsla(", "rgb(", "hsl("} {
		if strings.HasPrefix(value[index:], prefix) {
			return true
		}
	}
	return false
}

// cssLengthUnitAt answers the length of the unit at index, or zero when what
// follows the number is not one. The unit must end the word: "12emphasis" is
// not twelve em.
func cssLengthUnitAt(value string, index int) int {
	for _, unit := range []string{"rem", "px", "em"} {
		if !strings.HasPrefix(value[index:], unit) {
			continue
		}
		after := index + len(unit)
		if after < len(value) && value[after] >= 'a' && value[after] <= 'z' {
			return 0
		}
		return len(unit)
	}
	return 0
}

// cssTokenCandidates answers which literals a project repeats often enough to
// be worth naming.
//
// Most used first, then alphabetically, which is the C's ordering and matters
// because the reply is bounded: a caller taking the first page takes the
// literals most worth naming rather than an arbitrary set.
func cssTokenCandidates(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	projectFilter, minCount, err :=
		db2contract.DecodeCssTokenCandidatesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	floor := int(minCount)
	if floor < cssTokenMinimumCount {
		floor = cssTokenMinimumCount
	}

	rows, queryErr := store.Query(ctx, cssTokenCandidatesQuery, projectFilter)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	counts := map[cssToken]int{}
	for rows.Next() {
		var value string
		if scanErr := rows.Scan(&value); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		scanCSSLiterals(value, counts)
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	candidates := make([]db2contract.CssTokenCandidatesRow, 0, len(counts))
	for token, count := range counts {
		if count < floor {
			continue
		}
		candidates = append(candidates, db2contract.CssTokenCandidatesRow{
			TokenValue: token.value,
			TokenKind:  token.kind,
			TokenCount: clampToU32(int64(count)),
		})
	}
	sort.Slice(candidates, func(first, second int) bool {
		if candidates[first].TokenCount != candidates[second].TokenCount {
			return candidates[first].TokenCount > candidates[second].TokenCount
		}
		return candidates[first].TokenValue < candidates[second].TokenValue
	})
	if ceiling := db2contract.CssTokenCandidatesMaxRows; len(candidates) > ceiling {
		candidates = candidates[:ceiling]
	}

	reply, encodeErr := db2contract.EncodeCssTokenCandidatesReply(candidates)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

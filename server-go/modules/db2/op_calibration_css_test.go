package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestArtifactWriteExRecordsWhichAttempt(t *testing.T) {
	// The whole difference from artifact_write: a caller re-writing after a
	// failed attempt records which attempt this is, so a backoff policy has
	// something to read.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactWriteExRequest(
		"artifact-1", "synthesis", "proposed", "project", "aimee", "jbailes",
		0.8, 3, `{"claim":"x"}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactWriteEx), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeArtifactWriteExReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.lastArgs[7] != int64(3) {
		t.Errorf("attempt count = %v", store.lastArgs[7])
	}
}

func TestArtifactWriteExFloorsTheAttemptAtOne(t *testing.T) {
	// An artifact exists because something produced it, so the first attempt is
	// one. The plain write hard-codes that; this variant takes the count, which
	// is what makes zero expressible and meaningless.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactWriteExRequest(
		"artifact-1", "synthesis", "", "", "", "", 0.8, 0, "{}")
	if err != nil {
		// The envelope may floor it first, which is the same answer earlier.
		return
	}
	if _, status := handler(
		invocation(db2contract.StageArtifactWriteEx), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[7] != int64(1) {
		t.Errorf("attempt count = %v, want the floor", store.lastArgs[7])
	}
}

func TestAuditEventsCollapseARetry(t *testing.T) {
	// The identifier is the caller's. An audit trail that double-counted a
	// retry would misstate how often something happened.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAuditEventWriteRequest(
		"audit-1", "artifact-1", "recall", "memory:4", "jbailes", "", "",
		0.9, 1, `{"state":"proposed"}`, `{"state":"committed"}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAuditEventWrite), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeAuditEventWriteReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL, "ON CONFLICT (id) DO NOTHING") {
		t.Errorf("a retry would be counted twice: %q", store.lastSQL)
	}
	// An absent snapshot arrives as an empty string, and an empty string is not
	// valid JSON -- casting it directly fails the insert.
	if !strings.Contains(store.lastSQL, "NULLIF($10, '')::jsonb") ||
		!strings.Contains(store.lastSQL, "NULLIF($11, '')::jsonb") {
		t.Errorf("an absent snapshot would fail the write: %q", store.lastSQL)
	}
	// The flag is a boolean column, so a boolean is what goes into it.
	if flagged, ok := store.lastArgs[8].(bool); !ok || !flagged {
		t.Errorf("flagged = %#v, want a boolean", store.lastArgs[8])
	}
	if store.lastArgs[5] != "user" {
		t.Errorf("scope kind = %v, want the narrowest default", store.lastArgs[5])
	}
}

func TestCalibrationAnswersEveryBucket(t *testing.T) {
	// A caller plotting calibration needs the gaps: a surface that has never
	// been confident is a different picture from one that is confident and
	// wrong, and a reply containing only the buckets with rows cannot tell them
	// apart.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(9), int64(7), int64(1), int64(8)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationAuditStatsRequest(
		"recall", "synthesis", "", "", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCalibrationAuditStats), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	buckets, decodeErr := db2contract.DecodeCalibrationAuditStatsReply(body)
	if decodeErr != nil ||
		len(buckets) != db2contract.CalibrationAuditStatsMaxRows {
		t.Fatalf("buckets = %d", len(buckets))
	}
	last := buckets[len(buckets)-1]
	if last.Alpha != 7 || last.Beta != 1 || last.SampleN != 8 {
		t.Fatalf("filled bucket = %+v", last)
	}
	if buckets[0].SampleN != 0 || buckets[0].RangeLo != 0 {
		t.Fatalf("empty bucket = %+v", buckets[0])
	}
	if last.RangeHi != 1 {
		t.Errorf("the ranges do not reach one: %v", last.RangeHi)
	}
	// A window nobody named still bounds the scan.
	if store.lastArgs[5] != int64(calibrationAuditStatsDefaultWindow) {
		t.Errorf("window = %v, want the default", store.lastArgs[5])
	}
	// A confidence of exactly one would land past the last bucket and a
	// negative one below the first, so both are clamped rather than dropped.
	if !strings.Contains(store.lastSQL, "WHEN applied_confidence >= 1.0 THEN $5 - 1") ||
		!strings.Contains(store.lastSQL, "WHEN applied_confidence < 0.0 THEN 0") {
		t.Errorf("the edge confidences are no longer clamped: %q", store.lastSQL)
	}
	// The window is applied before the bucketing: limiting after would take a
	// slice of the buckets rather than of the judgements.
	if !strings.Contains(store.lastSQL, "ORDER BY ae.applied_at DESC\n   LIMIT $6") {
		t.Errorf("the window moved after the bucketing: %q", store.lastSQL)
	}
}

func TestCSSScannerFindsColoursAndLengths(t *testing.T) {
	// A value like "1px solid #fff" contributes two, which is why the counting
	// cannot move into SQL.
	counts := map[cssToken]int{}
	scanCSSLiterals("1px solid #FFF", counts)
	if counts[cssToken{"1px", "length"}] != 1 ||
		counts[cssToken{"#fff", "color"}] != 1 {
		t.Fatalf("counts = %v", counts)
	}
	// The functional forms have their interior whitespace removed, so two
	// spellings of the same colour are one literal.
	counts = map[cssToken]int{}
	scanCSSLiterals("rgb(0, 0, 0)", counts)
	scanCSSLiterals("rgb(0,0,0)", counts)
	if counts[cssToken{"rgb(0,0,0)", "color"}] != 2 {
		t.Fatalf("counts = %v", counts)
	}
}

func TestCSSScannerRefusesWhatIsNotALiteral(t *testing.T) {
	counts := map[cssToken]int{}
	// A hex run of the wrong length is not a colour.
	scanCSSLiterals("#ff #fffff", counts)
	// Zero needs no token and is written without a unit as often as with one.
	scanCSSLiterals("0px 0.0em", counts)
	// The unit must end the word.
	scanCSSLiterals("12emphasis", counts)
	// A number inside a word is not a length.
	scanCSSLiterals("grid12px", counts)
	if len(counts) != 0 {
		t.Fatalf("counts = %v", counts)
	}
}

func TestTokenCandidatesLeadWithWhatIsMostRepeated(t *testing.T) {
	// The reply is bounded, so a caller taking the first page should take the
	// literals most worth naming rather than an arbitrary set. A literal used
	// once is a one-off, not a candidate.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"1px solid #fff"}, {"2px solid #fff"}, {"#fff"}, {"3px"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssTokenCandidatesRequest("aimee", 2)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCssTokenCandidates), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	candidates, decodeErr := db2contract.DecodeCssTokenCandidatesReply(body)
	if decodeErr != nil || len(candidates) != 1 {
		t.Fatalf("candidates = %+v", candidates)
	}
	if candidates[0].TokenValue != "#fff" || candidates[0].TokenKind != "color" ||
		candidates[0].TokenCount != 3 {
		t.Fatalf("candidate = %+v", candidates[0])
	}
	if !strings.Contains(store.lastSQL, "f.generation = p.current_generation") {
		t.Errorf("a superseded stylesheet would be counted: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "($1 = '' OR p.name = $1)") {
		t.Errorf("the project filter changed shape: %q", store.lastSQL)
	}
}

func TestTokenCandidateFloorIsTwoWhateverIsAsked(t *testing.T) {
	// A literal used once is not a candidate for a design token, so a caller
	// asking for one gets the floor.
	store := &fakeStore{rows: &fakeRows{values: [][]any{{"#fff"}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCssTokenCandidatesRequest("", 1)
	if err != nil {
		// The envelope may floor it first, which is the same answer earlier.
		return
	}
	body, status := handler(invocation(db2contract.StageCssTokenCandidates), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	candidates, decodeErr := db2contract.DecodeCssTokenCandidatesReply(body)
	if decodeErr != nil || len(candidates) != 0 {
		t.Fatalf("a single use became a candidate: %+v", candidates)
	}
}

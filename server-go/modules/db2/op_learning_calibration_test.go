package db2

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestRuleWithoutAPolarityBecomesPositive(t *testing.T) {
	// The column has no default of its own, and the envelope allows an empty
	// polarity -- so unlike most of the C's defaults this one is reachable. A
	// rule stored without a polarity renders without the symbol that says
	// whether it is a do or a do-not.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesInsertRequest("", "write it down", "", 60)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRulesInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeRulesInsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.lastArgs[0] != "positive" {
		t.Errorf("polarity = %v, want the default", store.lastArgs[0])
	}
}

func TestReinforcingWithoutAWeightLeavesTheWeightAlone(t *testing.T) {
	// Leaving the weight out is not the same as setting it to zero: a rule
	// reinforced without a new weight keeps the one it earned. Two statements
	// rather than one is what makes that expressible.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesReinforceDirectiveRequest(4, "hard", 0, 90)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRulesReinforceDirective), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if strings.Contains(store.lastSQL, "weight = ") {
		t.Errorf("the weight was overwritten unasked: %q", store.lastSQL)
	}

	store = &fakeStore{}
	handler = NewDispatchHandler(store)
	request, err = db2contract.EncodeRulesReinforceDirectiveRequest(4, "hard", 1, 90)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRulesReinforceDirective), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "weight = $2") {
		t.Errorf("the weight was not set when asked: %q", store.lastSQL)
	}
	if store.lastArgs[1] != int64(90) {
		t.Errorf("weight = %v, want 90", store.lastArgs[1])
	}
}

func TestReinforcingStampsBothTimestamps(t *testing.T) {
	// They answer different questions -- when the row last changed, and when
	// someone last said the rule still holds -- and a decay pass reads the
	// second one.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRulesReinforceDirectiveRequest(4, "soft", 0, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRulesReinforceDirective), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "last_reinforced_at = pg_now_text()") {
		t.Errorf("the reinforcement is not recorded: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "updated_at = pg_now_text()") {
		t.Errorf("the row's own timestamp is not touched: %q", store.lastSQL)
	}
}

func TestDemotionProfileIsWrittenCommittedUnderTheScopeGiven(t *testing.T) {
	// The read that finds a profile filters on target_surface, so a profile
	// inserted without one is invisible to it. Setting it in the insert rather
	// than in a follow-up update is what closes the window where the row exists
	// and cannot be found.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionProfileWriteRequest(
		"episodic", "", "", `{"half_life_days":30}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionProfileWrite), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	profileID, decodeErr := db2contract.DecodeDemotionProfileWriteReply(body)
	if decodeErr != nil || profileID == "" {
		t.Fatalf("profile id = %q", profileID)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d, want one insert", store.execCalls)
	}
	if !strings.Contains(store.lastSQL, "'demotion_profile', 'committed'") {
		t.Errorf("a profile is no longer written committed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "target_surface") ||
		store.lastArgs[3] != "episodic" {
		t.Errorf("the surface is not set at insert: %q / %v",
			store.lastSQL, store.lastArgs)
	}
	// An empty scope kind is stored empty rather than promoted to "global".
	// The C promotes only a NULL one, and nothing on this wire can send NULL,
	// so reading "" as global here would be this port's own invention -- and it
	// would turn a write whose scope the caller left unset into a default
	// applying to every scope there is. The profile is then reachable only by
	// an exact-scope read, which is a real gap, and a smaller one than
	// silently widening a request nobody made.
	if store.lastArgs[1] != "" {
		t.Errorf("scope kind = %v, want it stored as it arrived",
			store.lastArgs[1])
	}
	if !strings.Contains(store.lastSQL, "$5::jsonb") {
		t.Errorf("the payload is not cast for a JSONB column: %q", store.lastSQL)
	}
}

func TestAttributionPayloadNamesItsFields(t *testing.T) {
	// The payload is built here rather than taken from the caller, so the four
	// fields are always spelled the same way for whoever reads them back. It
	// has to be valid JSON, because the column is JSONB and the insert casts.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRetrievalAttributionWriteRequest(
		"event-1", 4321, "accepted", 0.25)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageRetrievalAttributionWrite), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeRetrievalAttributionWriteReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	payload, ok := store.lastArgs[3].(string)
	if !ok {
		t.Fatalf("payload = %v", store.lastArgs[3])
	}
	var decoded struct {
		RetrievalEventID string  `json:"retrieval_event_id"`
		SurfacedRowID    int64   `json:"surfaced_row_id"`
		Verdict          string  `json:"verdict"`
		Weight           float64 `json:"weight"`
	}
	if jsonErr := json.Unmarshal([]byte(payload), &decoded); jsonErr != nil {
		t.Fatalf("the payload is not JSON: %v (%s)", jsonErr, payload)
	}
	if decoded.RetrievalEventID != "event-1" || decoded.SurfacedRowID != 4321 ||
		decoded.Verdict != "accepted" || decoded.Weight != 0.25 {
		t.Fatalf("payload = %s", payload)
	}
	// The row is also reachable two other ways: by row through scope_id, and
	// by event through model_version. The C sets the second in a follow-up
	// update it is willing to see fail, which leaves an attribution nothing
	// can join back to the event.
	if store.lastArgs[1] != "4321" {
		t.Errorf("scope id = %v, want the surfaced row as text", store.lastArgs[1])
	}
	if store.lastArgs[2] != "event-1" {
		t.Errorf("model version = %v, want the retrieval event", store.lastArgs[2])
	}
}

func TestAttributionQuotesWhatItInterpolates(t *testing.T) {
	// The payload is built by hand, so a verdict or an event identifier
	// carrying a quote would otherwise produce a string that is not JSON --
	// and the insert's cast would reject the whole write.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeRetrievalAttributionWriteRequest(
		`event"1`, 1, `acc"epted`, 0.5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageRetrievalAttributionWrite), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	payload, _ := store.lastArgs[3].(string)
	var decoded map[string]any
	if jsonErr := json.Unmarshal([]byte(payload), &decoded); jsonErr != nil {
		t.Fatalf("a quote broke the payload: %v (%s)", jsonErr, payload)
	}
	if decoded["verdict"] != `acc"epted` {
		t.Fatalf("verdict = %v", decoded["verdict"])
	}
}

func TestArtifactIdentifiersDiffer(t *testing.T) {
	// Two profiles written in a row must not collide: the insert does nothing
	// on conflict, so a repeated identifier would silently write one profile
	// and report two.
	seen := map[string]bool{}
	for attempt := 0; attempt < 32; attempt++ {
		id, err := newArtifactID()
		if err != nil {
			t.Fatalf("mint: %v", err)
		}
		if seen[id] {
			t.Fatalf("identifier repeated: %s", id)
		}
		seen[id] = true
		if len(id) != 36 || strings.Count(id, "-") != 4 {
			t.Fatalf("identifier is not the shape the C emits: %s", id)
		}
	}
}

func TestCalibrationCountsOnlyJudgedEvents(t *testing.T) {
	// An audit event with no verdict is one nobody has judged. Counting it
	// would let a surface reach the threshold on reviews that never happened,
	// and calibration would then trust a surface on the strength of its own
	// output.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"recall", "synthesis", "project", "aimee", int64(12)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationSurfaceListRequest(5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCalibrationSurfaceList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	surfaces, decodeErr := db2contract.DecodeCalibrationSurfaceListReply(body)
	if decodeErr != nil || len(surfaces) != 1 {
		t.Fatalf("rows = %+v", surfaces)
	}
	if surfaces[0].TargetSurface != "recall" || surfaces[0].ArtifactKind != "synthesis" ||
		surfaces[0].ScopeKind != "project" || surfaces[0].ScopeID != "aimee" {
		t.Fatalf("row = %+v", surfaces[0])
	}
	if !strings.Contains(store.lastSQL, "ae.verdict <> ''") {
		t.Errorf("unjudged events would count: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "HAVING COUNT(*) >= $1") {
		t.Errorf("HAVING names an alias PostgreSQL rejects there: %q", store.lastSQL)
	}
	if strings.Contains(store.lastSQL, "COALESCE") {
		t.Errorf("a COALESCE over a NOT NULL column is back: %q", store.lastSQL)
	}
}

func TestCalibrationListFloorsItsThreshold(t *testing.T) {
	// Zero would admit every group, which is the opposite of what a caller
	// asking for a threshold wants.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationSurfaceListRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCalibrationSurfaceList), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[0] != int64(1) {
		t.Errorf("threshold = %v, want 1", store.lastArgs[0])
	}
}

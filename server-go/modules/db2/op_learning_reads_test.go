package db2

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestBanditArmsListEscapesWhatItEmits(t *testing.T) {
	// The C formats each element as "%s" with no escaping, so an arm containing
	// a quote produces a document its own caller cannot parse. Building it with
	// encoding/json is a fix rather than a divergence -- reproducing the C would
	// mean writing broken JSON on purpose.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{`arm "quoted"`}, {`arm\with\backslashes`},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmsListRequest("replay-point")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmsList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	arms, decodeErr := db2contract.DecodeBanditArmsListReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	var parsed []string
	if err := json.Unmarshal([]byte(arms), &parsed); err != nil {
		t.Fatalf("the reply is not parseable JSON: %v (%q)", err, arms)
	}
	if len(parsed) != 2 || parsed[0] != `arm "quoted"` {
		t.Fatalf("round trip lost the value: %#v", parsed)
	}
}

func TestBanditArmsListAnswersAnEmptyArrayNotAnEmptyString(t *testing.T) {
	// A caller parses this unconditionally, so "nothing" has to be a document.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmsListRequest("never-decided")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmsList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	arms, decodeErr := db2contract.DecodeBanditArmsListReply(body)
	if decodeErr != nil || arms != "[]" {
		t.Fatalf("arms = %q, want %q", arms, "[]")
	}
}

func TestBanditArmsListReadsTheDecisionsNotARegisterOfArms(t *testing.T) {
	// An arm that has never been chosen does not appear. That is the question
	// being asked -- what does this decision point actually do -- and it means a
	// newly added arm is invisible until first tried.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmsListRequest("replay-point")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageBanditArmsList), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "FROM bandit_decisions") {
		t.Errorf("the arms are no longer read from the decisions: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY arm_id") {
		t.Error("the set is not stably ordered; two reads would not compare")
	}
}

func TestCalibrationFloorsTheThresholdAtOne(t *testing.T) {
	// HAVING COUNT(*) >= 0 admits every group, including those with nothing to
	// calibrate against, so the C floors it.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(4)}}}
	if _, status := calibrationSurfacesWithData(t.Context(), store,
		mustEncode(t, func() ([]byte, error) {
			return db2contract.EncodeCalibrationSurfacesWithDataRequest(
				db2contract.CalibrationSurfacesWithDataMinRowsMin)
		})); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 1 {
		t.Fatalf("args = %v", store.lastArgs)
	}
	if threshold, ok := store.lastArgs[0].(int64); !ok || threshold < 1 {
		t.Fatalf("threshold = %v, want at least 1", store.lastArgs[0])
	}
}

func TestCalibrationGroupsBySurfaceKindAndScope(t *testing.T) {
	// A surface calibrates separately per artifact kind and per scope, because
	// a verdict on one says little about another. Collapsing the grouping would
	// report far fewer surfaces as having data and silently widen calibration.
	store := &fakeStore{row: &fakeRow{values: []any{idPtr(0)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationSurfacesWithDataRequest(3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageCalibrationSurfacesWithData), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL,
		"GROUP BY ae.target_surface, a.kind, ae.scope_kind, ae.scope_id") {
		t.Errorf("the grouping changed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "ae.verdict <> ''") {
		t.Error("unjudged events now count as evidence")
	}
}

func TestAuditLatestBeforeOrdersByIdNotTime(t *testing.T) {
	// This is what an undo reads. Two events stamped the same second still
	// order by id; ordering by a timestamp would leave the choice between them
	// to the planner, and the wrong one of two is worse than none.
	store := &fakeStore{row: &fakeRow{values: []any{ptr("{}")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAuditLatestBeforeRequest("artifact-1")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageAuditLatestBefore), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY id DESC LIMIT 1") {
		t.Errorf("the most recent event is no longer chosen by id: %q", store.lastSQL)
	}
}

func TestPromotionAndSurfaceAnswerEmptyWhenAbsent(t *testing.T) {
	// Absence is an answer to both: a decision point still exploring has no
	// promotion, and an artifact nothing holds has no surface.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		read  func([]byte) (string, error)
	}{
		{
			"bandit_promotion_get",
			db2contract.StageBanditPromotionGet,
			func() ([]byte, error) {
				return db2contract.EncodeBanditPromotionGetRequest("still-exploring")
			},
			db2contract.DecodeBanditPromotionGetReply,
		},
		{
			"artifact_target_surface",
			db2contract.StageArtifactTargetSurface,
			func() ([]byte, error) {
				return db2contract.EncodeArtifactTargetSurfaceRequest("no-such-artifact")
			},
			db2contract.DecodeArtifactTargetSurfaceReply,
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			handler := NewDispatchHandler(&fakeStore{})
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			value, decodeErr := testCase.read(body)
			if decodeErr != nil || value != "" {
				t.Fatalf("value = %q, want empty", value)
			}
		})
	}
}

func TestEvidencePendingListBoundsItselfAndOrdersByArtifact(t *testing.T) {
	// The request carries no limit, so the ceiling is the reply's own. Ordered
	// by artifact rather than by age, so a caller draining this sees one
	// artifact's work together.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{ptr("artifact-1"), ptr("evidence")},
		{ptr("artifact-2"), (*string)(nil)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEvidencePendingListRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageEvidencePendingList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, decodeErr := db2contract.DecodeEvidencePendingListReply(body)
	if decodeErr != nil {
		t.Fatalf("decode reply: %v", decodeErr)
	}
	if len(found) != 2 || found[1].Collection != "" {
		t.Fatalf("rows = %+v", found)
	}
	if !strings.Contains(store.lastSQL, "ORDER BY artifact_id") {
		t.Errorf("the ordering changed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "status = 'pending'") {
		t.Error("the read is no longer restricted to pending work")
	}
	if len(store.lastArgs) != 1 ||
		store.lastArgs[0] != int64(db2contract.EvidencePendingListMaxRows) {
		t.Fatalf("args = %v -- the limit is not the reply's own ceiling", store.lastArgs)
	}
}

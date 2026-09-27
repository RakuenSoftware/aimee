package memory

import (
	"encoding/json"
	"math"
	"reflect"
	"testing"
)

func TestHealthNativeRankingJoinsExactOwnerAndRevision(t *testing.T) {
	version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "7", RecordRevision: "3"}
	source := &typedSourceVersion{Kind: "user_memory_record", Version: version}
	record := healthRecord{RecordID: healthSourceIdentity(source), VersionID: "3", Kind: "fact", SelectionPaths: []string{"native_active_context"}}
	step := rankingStep{Operation: "rrf60", Score: 1.0 / 61, Contributions: []rankingContribution{{Arm: "semantic", Rank: 1, Value: 1.0 / 61}}}
	capture := rankingCapture{SchemaVersion: 1, Candidates: []rankingCandidate{{ID: "7", Version: &version, Steps: []rankingStep{step}}}}
	raw, _ := json.Marshal(capture)
	got := healthNativeRanking([]healthRecord{record}, raw, "user")
	if len(got[0].RankingSteps) != 1 || !reflect.DeepEqual(got[0].RankingSteps[0], step) || !healthRecordArmsKnown(got[0]) {
		t.Fatal(got)
	}
	if got := healthNativeRanking([]healthRecord{record}, raw, "kb"); len(got[0].RankingSteps) != 0 {
		t.Fatal("scores crossed placements with equal owner IDs")
	}
	for _, field := range []string{"owner", "revision"} {
		changed := record
		if field == "owner" {
			foreign := *source
			foreign.Version.OwnerID = "00000000-0000-4000-8000-000000000002"
			changed.RecordID = healthSourceIdentity(&foreign)
		} else {
			changed.VersionID = "4"
		}
		if got := healthNativeRanking([]healthRecord{changed}, raw, "user"); len(got[0].RankingSteps) != 0 || healthRecordArmsKnown(got[0]) {
			t.Fatal("foreign score imported", got)
		}
	}
}
func TestHealthNativeSelectorsDoNotInventScores(t *testing.T) {
	if !healthRecordArmsKnown(healthRecord{SelectionPaths: []string{"native_identity", "native_preferences"}}) {
		t.Fatal("unranked selectors not recognized")
	}
	if healthRecordArmsKnown(healthRecord{SelectionPaths: []string{"native_identity", "native_active_context"}}) {
		t.Fatal("active context score invented")
	}
	if validatedHealthSelectionPaths([]string{"invented"}) != nil {
		t.Fatal("unknown selector accepted")
	}
	if validatedHealthRanking([]rankingStep{{Operation: "rrf60", Score: math.NaN(), Contributions: []rankingContribution{{Arm: "lexical", Value: 1}}}}) != nil {
		t.Fatal("invalid score accepted")
	}
}
func TestHealthNativeRankingMetadataLeavesProviderProjectionUnchanged(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	b := nativeRecallFixture()
	version := &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "7", RecordRevision: "3"}
	b.ActiveContext = recallItems([]Record{{ID: 7, Scope: Scope{Type: ScopeUser, Value: "_user"}, Kind: "fact", Content: "served source", Version: version}})
	envelope := map[string]any{"status": "ok", "recall": b}
	raw, _ := json.Marshal(envelope)
	args := sourceReleaseArgs(map[string]any{"native_context_bytes": 4096})
	before, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		t.Fatal(err)
	}
	envelope["health_ranking_store"] = "user"
	envelope["health_ranking"] = rankingCapture{SchemaVersion: 1, Candidates: []rankingCandidate{{ID: "7", Version: version, Steps: []rankingStep{{Operation: "native_cosine_similarity", Score: .9, Contributions: []rankingContribution{{Arm: "semantic", Rank: 1, Value: .9}}}}}}}
	raw, _ = json.Marshal(envelope)
	after, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		t.Fatal(err)
	}
	var old, current struct {
		Projection nativeRecallProjection `json:"native_context"`
	}
	json.Unmarshal(before, &old)
	json.Unmarshal(after, &current)
	if old.Projection.Text != current.Projection.Text || old.Projection.SourceDigest != current.Projection.SourceDigest || old.Projection.SelectionDigest != current.Projection.SelectionDigest || len(current.Projection.HealthRecords) != 1 || len(current.Projection.HealthRecords[0].RankingSteps) != 1 {
		t.Fatal("optional ranking changed serving or was lost")
	}
}

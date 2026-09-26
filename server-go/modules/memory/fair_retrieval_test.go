package memory

import (
	"context"
	"testing"
)

func TestFairPageRankFullPoolAdmitsGraphOnly(t *testing.T) {
	base := make([]Record, pageRankCandidateCap)
	for i := range base {
		base[i].ID = int64(i + 1)
	}
	graph := []Record{{ID: 1000}, {ID: 1001}, {ID: 1000}, {ID: 1}}
	got := fairPageRankPool(context.Background(), base, graph)
	if len(got) != pageRankCandidateCap || got[len(got)-2].ID != 1000 || got[len(got)-1].ID != 1001 {
		t.Fatal("full initial pool vetoed graph", got)
	}
	got = fairPageRankPool(context.Background(), base, nil)
	if len(got) != len(base) || got[len(got)-1].ID != base[len(base)-1].ID {
		t.Fatal("unused graph quota wasted")
	}
	many := make([]Record, 200)
	for i := range many {
		many[i].ID = int64(1000 + i)
	}
	got = fairPageRankPool(context.Background(), base, many)
	if len(got) != 128 || got[63].ID != 64 || got[64].ID != 1000 {
		t.Fatal("arm or work cap exceeded")
	}
}
func TestFairNativeFusionRefusesMixedVersions(t *testing.T) {
	for _, different := range []string{"revision", "owner", "unknown"} {
		t.Run(different, func(t *testing.T) {
			version := &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "owner", RecordID: "1", RecordRevision: "1"}
			other := *version
			a, b := Record{ID: 1, observedVersion: version}, Record{ID: 1, Version: &other}
			switch different {
			case "revision":
				other.RecordRevision = "2"
			case "owner":
				other.OwnerID = "foreign"
			case "unknown":
				b.Version = nil
			}
			ctx := withRankingTrace(context.Background())
			got := fuseRanked(ctx, []Record{a, {ID: 2}}, []Record{b, {ID: 3}}, 3, "lexical", "semantic")
			if len(got) != 2 {
				t.Fatal("mixed-version candidate survived", got)
			}
			for _, r := range got {
				if r.ID == 1 {
					t.Fatal(r)
				}
			}
			capture := finishRankingCapture(ctx, got)
			found := false
			for _, c := range capture.Candidates {
				found = found || c.ID == "1" && c.Disposition == "conflicting_source_version"
			}
			if !found {
				t.Fatal("version refusal missing from trace", capture)
			}
		})
	}
}
func TestRetrievalCapabilitiesFallbackAndNestedIsolation(t *testing.T) {
	root := context.Background()
	ctx := withRetrievalCapabilities(root, PlacementServer, DataRequest{Operation: "search"})
	recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "unavailable", Reason: "local_embedder_not_configured"})
	recordRetrievalArm(ctx, "lexical", retrievalArmObservation{State: "available", Candidates: 2, Quota: 2})
	child := withRetrievalCapabilities(ctx, PlacementKB, DataRequest{Operation: "assertion-search"})
	recordRetrievalArm(child, "dense", retrievalArmObservation{State: "available", Candidates: 1, Quota: 64})
	a, b := observedRetrievalCapabilities(ctx), observedRetrievalCapabilities(child)
	if a.DeclaredArms["graph"] || a.DeclaredArms["code"] || a.Arms["dense"][0].State != "unavailable" || b.Arms["dense"][0].State != "available" || b.DeclaredArms["code"] || observedRetrievalCapabilities(root) != nil {
		t.Fatal(a, b)
	}
	// Repeated calls and mixed readiness are retained, not last-call-wins.
	recordRetrievalArm(child, "dense", retrievalArmObservation{State: "unavailable", Reason: "budget"})
	recordRetrievalArm(child, "dense", retrievalArmObservation{State: "available", Candidates: 1, Quota: 64})
	got := observedRetrievalCapabilities(child)
	if len(got.Arms["dense"]) != 2 || got.Arms["dense"][0].Calls != 2 || len(b.Arms["dense"]) != 1 {
		t.Fatal(got)
	}
}

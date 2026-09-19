package memory

import (
	"context"
	"reflect"
	"sync"
	"testing"
)

// Port of the retired, unregistered native test_memory_lane_outcome.c fixture.
func TestLaneOutcomesFinalWindow(t *testing.T) {
	sources := recallLanes{1: laneSemantic, 2: laneSemantic | laneLexical, 3: laneLexical, 4: laneGraph}
	got := laneOutcomes("hybrid", []Record{{ID: 1}, {ID: 2}, {ID: 3}, {ID: 4}}, 2, sources)
	want := map[string]int64{
		"memory.query.lane.semantic.candidates": 2, "memory.query.lane.semantic.served": 2,
		"memory.query.lane.lexical.candidates": 2, "memory.query.lane.lexical.served": 1,
		"memory.query.lane.graph.candidates": 1, "memory.query.lane.graph.served": 0,
		"memory.query.lane.graph.shutout":                1,
		"memory.query.route.hybrid.lane.semantic.served": 2,
		"memory.query.route.hybrid.lane.lexical.served":  1,
		"memory.query.route.hybrid.lane.graph.served":    0,
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("lane outcomes = %#v; want %#v", got, want)
	}
	if got := laneOutcomes("", []Record{{ID: 7}}, 1, recallLanes{7: laneUnit}); !reflect.DeepEqual(got, map[string]int64{
		"memory.query.lane.unit.candidates": 1, "memory.query.lane.unit.served": 1,
	}) {
		t.Fatal(got)
	}
}

func TestLaneOutcomesEmptyAndDegenerate(t *testing.T) {
	if got := laneOutcomes("", []Record{{ID: 1}}, 1, nil); len(got) != 0 {
		t.Fatal(got)
	}
	for _, test := range []struct {
		matches []Record
		served  int
	}{{[]Record{{ID: 1}}, 0}, {[]Record{{ID: 1}}, -3}, {nil, 5}} {
		got := laneOutcomes("", test.matches, test.served, recallLanes{1: laneLexical})
		want := map[string]int64{"memory.query.lane.lexical.candidates": 1, "memory.query.lane.lexical.served": 0, "memory.query.lane.lexical.shutout": 1}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("got %v want %v", got, want)
		}
	}
}

func TestLaneAttributionDeduplicatesAndSnapshotsConcurrently(t *testing.T) {
	lanes := recallLanes{}
	lanes.add([]Record{{ID: 2}, {ID: 2}}, laneLexical)
	lanes.add([]Record{{ID: 2}}, laneSemantic)
	if len(lanes) != 1 || lanes[2] != laneLexical|laneSemantic {
		t.Fatal(lanes)
	}
	before := laneMetrics()["memory.query.lane.semantic.served"]
	var wg sync.WaitGroup
	for range 10 {
		wg.Add(1)
		go func() { defer wg.Done(); lanes.observe([]Record{{ID: 2}}); _ = laneMetrics() }()
	}
	wg.Wait()
	if got := laneMetrics()["memory.query.lane.semantic.served"]; got != before+10 {
		t.Fatalf("served = %d, want %d", got, before+10)
	}
	snapshot := laneMetrics()
	snapshot["memory.query.lane.semantic.served"] = -1
	if laneMetrics()["memory.query.lane.semantic.served"] < 0 {
		t.Fatal("snapshot aliases internal counters")
	}
}

func TestRecallLaneOutcomesRequireSuccessfulStoreSelection(t *testing.T) {
	before := laneMetrics()
	failing := &postgresDataStore{placement: PlacementKB, requireSemantic: true}
	req := DataRequest{Query: "needle", Limit: 1}
	if _, err := failing.finalizeRecall(context.Background(), req, true, []Record{{ID: 1}}); err == nil {
		t.Fatal("required semantic dependency failure was hidden")
	}
	if after := laneMetrics(); !reflect.DeepEqual(before, after) {
		t.Fatal("failed recall published a selected-result metric", before, after)
	}
	private := &postgresDataStore{placement: PlacementServer}
	if result, err := private.finalizeRecall(context.Background(), req, true, []Record{{ID: 1}}); err != nil || len(result) != 1 {
		t.Fatal(result, err)
	}
	if after := laneMetrics(); after["memory.query.lane.lexical.served"] != before["memory.query.lane.lexical.served"]+1 {
		t.Fatal("successful lexical result missing from counters", before, after)
	}
}

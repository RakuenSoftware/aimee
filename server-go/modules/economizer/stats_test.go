package economizer

import (
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestStatsPublishesEveryCounterEvenAtZero(t *testing.T) {
	// A panel that vanishes when the lever is idle reads as "the metric broke",
	// so every known counter is published whether or not it has fired.
	snap := NewGatewayStatsStore().Snapshot()
	for _, published := range statPublishedName {
		if _, ok := snap.Counters[published]; !ok {
			t.Errorf("%s missing from an idle snapshot", published)
		}
	}
	if snap.TokenDelta.SampleRate1N != statTokenSampleN {
		t.Errorf("sample rate = %d", snap.TokenDelta.SampleRate1N)
	}
	if snap.TokenDelta.PctReduced != 0 {
		t.Error("pct_reduced must be 0 before the first sample, not a divide by zero")
	}
}

func TestStatsReasonRegistryOverflowsRatherThanDrops(t *testing.T) {
	s := NewGatewayStatsStore()
	for i := 0; i < statReasonMax+50; i++ {
		s.IncReason("hard_bypass", string(rune('a'+i%26))+string(rune(i)))
	}
	snap := s.Snapshot()
	var total uint64
	for _, n := range snap.Reasons["hard_bypass"] {
		total += n
	}
	// Every event is still counted somewhere, even once the table is full.
	if total != uint64(statReasonMax+50) {
		t.Errorf("counted %d of %d events", total, statReasonMax+50)
	}
	if snap.Reasons["hard_bypass"]["_overflow"] == 0 {
		t.Error("the overflow row should be carrying the excess")
	}
}

// Carried over from the C this replaced (test_token_delta_sampling), case for
// case: WHICH call is sampled is the property, not just how many. The gate is
// deterministic on the count seen, so the first is taken and the next N-1 are
// not — get that off by one and the sums describe different turns than the
// count claims.
func TestStatsTokenDeltaSamples(t *testing.T) {
	s := NewGatewayStatsStore()

	s.RecordTokenDelta(1000, 500) // n=0 is sampled
	snap := s.Snapshot()
	if snap.TokenDelta.SampleCount != 1 ||
		snap.TokenDelta.BaselineSum != 1000 || snap.TokenDelta.ReducedSum != 500 {
		t.Fatalf("first call should be the sample: %+v", snap.TokenDelta)
	}

	for i := 0; i < statTokenSampleN-1; i++ {
		s.RecordTokenDelta(2000, 1900)
	}
	if got := s.Snapshot().TokenDelta.SampleCount; got != 1 {
		t.Errorf("n=1..%d must not be sampled, count = %d", statTokenSampleN-1, got)
	}

	s.RecordTokenDelta(4000, 1000) // n=N -> sampled again
	snap = s.Snapshot()
	if snap.TokenDelta.SampleCount != 2 ||
		snap.TokenDelta.BaselineSum != 5000 || snap.TokenDelta.ReducedSum != 1500 {
		t.Errorf("second sample: %+v", snap.TokenDelta)
	}
	// The property the sums exist to answer: sampled reduced is below baseline.
	if snap.TokenDelta.ReducedSum >= snap.TokenDelta.BaselineSum {
		t.Error("sampled reduction should shrink the transcript")
	}
	if snap.TokenDelta.PctReduced != 70 {
		t.Errorf("pct_reduced = %v, want 70", snap.TokenDelta.PctReduced)
	}

	// A negative reading is rejected rather than accumulated as a huge unsigned.
	s.RecordTokenDelta(-1, 5)
	if s.Snapshot().TokenDelta.BaselineSum != 5000 {
		t.Error("a negative baseline must not be accumulated")
	}
}

func statsSnapshot(t *testing.T, h bus.ModuleHandler, req StatsRequest) StatsSnapshot {
	t.Helper()
	body, err := json.Marshal(req)
	if err != nil {
		t.Fatal(err)
	}
	out, st := h(bus.ModuleInvocation{StageID: StageStats}, body)
	if st != bus.ModuleStatusOK {
		t.Fatalf("status = %v", st)
	}
	var snap StatsSnapshot
	if err := json.Unmarshal(out, &snap); err != nil {
		t.Fatal(err)
	}
	return snap
}

// The counters are only worth moving if the decisions actually reach them, so
// this drives real work through the handler and reads the published snapshot
// back out — the same way the HTTP surface will.
func TestStatsStageCountsWhatTheModuleDecided(t *testing.T) {
	h := NewHandler()

	body, err := json.Marshal(ReduceRequest{
		Messages: rawMessages(t, 20), SystemPrompt: "sys", Seam: "gateway",
		SessionKey: "sess", HistoryFold: true, ClosetEnabled: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, st := h(bus.ModuleInvocation{StageID: StageReduce}, body); st != bus.ModuleStatusOK {
		t.Fatalf("reduce status = %v", st)
	}

	snap := statsSnapshot(t, h, StatsRequest{Op: "snapshot"})
	if snap.Counters["gateway_mutate_attempted"] != 1 {
		t.Errorf("attempted = %d, want 1", snap.Counters["gateway_mutate_attempted"])
	}
	if snap.Counters["gateway_mutate_applied"] != 1 {
		t.Errorf("applied = %d, want 1", snap.Counters["gateway_mutate_applied"])
	}

	// A 4xx trips the breaker, which is counted where it is decided.
	postStatus(t, h, PostStatusRequest{
		SessionKey: "sess", HTTPStatus: 413, Mutated: true, TTLMS: 60_000,
	})
	snap = statsSnapshot(t, h, StatsRequest{Op: "snapshot"})
	if snap.Counters["gateway_4xx_restore_resend"] != 1 {
		t.Errorf("4xx counter = %d", snap.Counters["gateway_4xx_restore_resend"])
	}
	if snap.Reasons["session_disabled_set"]["4xx"] != 1 {
		t.Errorf("session_disabled_set{4xx} = %v", snap.Reasons["session_disabled_set"])
	}

	// The next turn is blocked by the breaker, and that has its own counter
	// rather than reading as a reduction fault.
	if _, st := h(bus.ModuleInvocation{StageID: StageReduce}, body); st != bus.ModuleStatusOK {
		t.Fatalf("reduce status = %v", st)
	}
	snap = statsSnapshot(t, h, StatsRequest{Op: "snapshot"})
	if snap.Counters["gateway_session_disabled_blocks"] != 1 {
		t.Errorf("blocks = %d, want 1", snap.Counters["gateway_session_disabled_blocks"])
	}
	if snap.Counters["gateway_mutate_attempted"] != 1 {
		t.Error("a blocked turn is not an attempt")
	}
}

// The stage is read-only: the caller has no counter to report, so there is no
// operation that can write one. Anything but a snapshot is refused rather than
// quietly treated as a read.
func TestStatsStageIsReadOnly(t *testing.T) {
	h := NewHandler()
	before := statsSnapshot(t, h, StatsRequest{Op: "snapshot"})
	// A bare request is a read, not a silent write.
	statsSnapshot(t, h, StatsRequest{})
	after := statsSnapshot(t, h, StatsRequest{Op: "snapshot"})
	for name, v := range before.Counters {
		if after.Counters[name] != v {
			t.Errorf("%s moved from %d to %d without any work being done", name, v, after.Counters[name])
		}
	}

	body, _ := json.Marshal(StatsRequest{Op: "inc"})
	if _, st := h(bus.ModuleInvocation{StageID: StageStats}, body); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("a write operation should be refused, got %v", st)
	}
}

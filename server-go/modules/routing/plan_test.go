package routing

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"math"
	"testing"
)

func rates(in, out float64) Prices { return Prices{Input: &in, Output: &out} }
func TestPlanFreeUnknownAndCompetence(t *testing.T) {
	req := PlanRequest{InputTokens: 1000, OutputTokens: 1000, Candidates: []Candidate{
		{Name: "unknown", Tier: 0, Competence: 90},
		{Name: "paid", Tier: 0, Competence: 90, Prices: rates(1, 3)},
		{Name: "free", Tier: 9, Competence: 80, Overrides: rates(0, 0)},
	}}
	if got := Plan(req); got != 2 {
		t.Fatalf("free qualified model lost: %d", got)
	}
	req.Premium = true
	if got := Plan(req); got != 1 {
		t.Fatalf("premium must choose qualified competence then known cost: %d", got)
	}
	req.Candidates[2].Competence = 90
	if got := Plan(req); got != 2 {
		t.Fatalf("free equal-competence model lost: %d", got)
	}
}
func TestPlanContextBandReversesOrdering(t *testing.T) {
	req := PlanRequest{InputTokens: 100, OutputTokens: 10, Candidates: []Candidate{
		{Name: "banded", Prices: rates(1, 1), Bands: []PriceBand{{Above: 100, Prices: rates(10, 10)}}},
		{Name: "flat", Prices: rates(2, 2)},
	}}
	if Plan(req) != 0 {
		t.Fatal("band must not apply at exact boundary")
	}
	req.InputTokens++
	if Plan(req) != 1 {
		t.Fatal("above-boundary full-request price must reverse ordering")
	}
	req.Candidates[0].Overrides = rates(0, 0)
	if Plan(req) != 0 {
		t.Fatal("explicit free override lost to band")
	}
}
func TestEstimateRejectsIncompleteAndInvalidPrices(t *testing.T) {
	for _, c := range []Candidate{
		{Prices: rates(1, 1), Truncated: true},
		{Prices: Prices{Input: rates(0, 0).Input}},
		{Prices: rates(-1, 1)}, {Prices: rates(math.Inf(1), 1)}, {Prices: rates(math.NaN(), 1)},
		{Prices: rates(math.MaxFloat64, 1)},
	} {
		if _, ok := Estimate(c, 1000, 100); ok {
			t.Fatalf("invalid/incomplete cost accepted: %+v", c)
		}
	}
	c := Candidate{Prices: rates(1, 1), Truncated: true, Overrides: rates(0, 0)}
	if cost, ok := Estimate(c, 1000, 100); !ok || cost != 0 {
		t.Fatal("complete explicit overrides must survive truncated catalog")
	}
	c = Candidate{Prices: rates(10, 20), Overrides: Prices{Input: rates(0, 0).Input}}
	if cost, ok := Estimate(c, 1000, 1000); !ok || cost != 0.02 {
		t.Fatalf("per-axis override: %g %v", cost, ok)
	}
}
func TestPlanDeterministicFallbackAndWire(t *testing.T) {
	req := PlanRequest{Version: 1, InputTokens: 1000, OutputTokens: 1000, Candidates: []Candidate{
		{Name: "z", Tier: 1}, {Name: "a", Tier: 1}, {Name: "expensive", Tier: 2},
	}}
	if Plan(req) != 1 {
		t.Fatal("unknown-cost fallback must use tiers and stable names")
	}
	req.Candidates[0], req.Candidates[1] = req.Candidates[1], req.Candidates[0]
	if Plan(req) != 0 {
		t.Fatal("fallback depends on roster ordering")
	}
	raw, _ := json.Marshal(req)
	reply, status := Handle(bus.ModuleInvocation{StageID: StagePlan}, raw)
	if status != bus.ModuleStatusOK || string(reply) != `{"selected":0}` {
		t.Fatalf("wire: %s %v", reply, status)
	}
	for _, raw := range []string{`{}`, `{"version":2}`, `{"version":1,"candidates":[]}`, `{"version":1,"candidates":[{"name":"a"},{"name":"a"}]}`} {
		if _, status := Handle(bus.ModuleInvocation{StageID: StagePlan}, []byte(raw)); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("accepted malformed plan", raw)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StagePlan, DeadlineNS: 1}, []byte("bad")); status != bus.ModuleStatusCancelled {
		t.Fatal("cancellation must precede parsing")
	}
}

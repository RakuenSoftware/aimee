package economizer

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
)

func taskCostFixture() TaskCostRequest {
	stages := map[string]string{}
	for _, s := range taskCostStages {
		stages[s] = "not_applicable:no invocation"
	}
	stages["generation"] = "observed"
	stages["verification"] = "observed"
	stages["indexing_embedding"] = "unknown"
	entry := TaskCostEntry{ID: "call-1", Attempt: "attempt-1", Stage: "generation", Kind: "actual", Nanodollars: "9007199254740993", Provider: "provider", Model: "model", PricingSnapshot: "invoice:1", CacheAssumptions: "invoice includes cache", Allocation: "marginal", AllocationRule: "entire call", Evidence: "receipt:1", LatencyMS: "25"}
	retry := entry
	retry.ID = "call-2"
	retry.Attempt = "attempt-2"
	retry.Kind = "estimated"
	retry.Nanodollars = "7"
	retry.PricingSnapshot = "rates:2026-09-27"
	verifier := entry
	verifier.ID = "verify-2"
	verifier.Attempt = "attempt-2"
	verifier.Stage = "verification"
	verifier.Kind = "unpriced"
	verifier.Nanodollars = ""
	return TaskCostRequest{SchemaVersion: 1, Task: "task", Stages: stages, Entries: []TaskCostEntry{entry, retry, verifier, entry}}
}
func TestTaskCostIncludesRetriesUnknownAndExactIntegerAmounts(t *testing.T) {
	req := taskCostFixture()
	out, err := ReportTaskCost(req)
	if err != nil {
		t.Fatal(err)
	}
	if out.Actual != "9007199254740993" || out.Estimated != "7" || out.KnownTotal != "9007199254741000" || len(out.Entries) != 3 || len(out.Attempts) != 2 || len(out.Unpriced) != 1 || len(out.UnknownStages) != 1 || out.Coverage != "incomplete" || out.PolicyAdmission != "observe_only_unqualified" {
		t.Fatalf("incorrect accounting: %+v", out)
	}
	raw, _ := json.Marshal(req)
	body, status := NewHandler()(bus.ModuleInvocation{StageID: StageTaskCost}, raw)
	if status != bus.ModuleStatusOK || !strings.Contains(string(body), `"9007199254741000"`) {
		t.Fatalf("wire amount lost: %s, %v", body, status)
	}
}
func TestTaskCostRejectsOmissionConflictAndOverflow(t *testing.T) {
	cases := map[string]func(*TaskCostRequest){
		"omitted stage":               func(r *TaskCostRequest) { delete(r.Stages, "tools") },
		"invented stage":              func(r *TaskCostRequest) { r.Entries[0].Stage = "hidden" },
		"false zero stage":            func(r *TaskCostRequest) { r.Stages["generation"] = "not_applicable:no call" },
		"empty observed stage":        func(r *TaskCostRequest) { r.Stages["tools"] = "observed" },
		"conflicting duplicate":       func(r *TaskCostRequest) { r.Entries[3].Nanodollars = "1" },
		"overflow":                    func(r *TaskCostRequest) { r.Entries = r.Entries[:3]; r.Entries[0].Nanodollars = "9223372036854775807" },
		"rounded number":              func(r *TaskCostRequest) { r.Entries[0].Nanodollars = "1e3" },
		"negative":                    func(r *TaskCostRequest) { r.Entries[0].Nanodollars = "-1" },
		"missing allocation":          func(r *TaskCostRequest) { r.Entries[0].AllocationRule = "" },
		"missing price snapshot":      func(r *TaskCostRequest) { r.Entries[1].PricingSnapshot = "" },
		"unpriced with invented zero": func(r *TaskCostRequest) { r.Entries[2].Nanodollars = "0" },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			r := taskCostFixture()
			mutate(&r)
			if _, err := ReportTaskCost(r); err == nil {
				t.Fatal("accepted invalid ledger")
			}
		})
	}
}
func TestTaskCostDoesNotPromoteDeclaredCompleteLedger(t *testing.T) {
	r := taskCostFixture()
	r.Stages["indexing_embedding"] = "not_applicable:external corpus"
	r.Entries = r.Entries[:2]
	r.Stages["verification"] = "not_applicable:unverified task"
	out, err := ReportTaskCost(r)
	if err != nil {
		t.Fatal(err)
	}
	if out.Coverage != "caller_declared" || out.PolicyAdmission != "observe_only_unqualified" {
		t.Fatal("declaration became a quality gate")
	}
	r.Entries[1].Allocation = "amortized"
	r.Entries[1].AllocationRule = "one of ten uses, rounded up"
	out, err = ReportTaskCost(r)
	if err != nil || out.Amortized != "7" || out.Marginal != out.Actual {
		t.Fatalf("allocation lost: %+v %v", out, err)
	}
}

func TestTaskCostIntegerRateModelRetainsCacheAndRounding(t *testing.T) {
	r := taskCostFixture()
	r.Entries = r.Entries[:3]
	r.Entries[1].Nanodollars = ""
	r.Entries[1].EstimatedUnits = []TaskCostUnits{{Kind: "input_uncached_tokens", Units: "100", Rate: "1000000"}, {Kind: "input_cached_tokens", Units: "100", Rate: "250000"}, {Kind: "output_tokens", Units: "3", Rate: "333333"}}
	out, err := ReportTaskCost(r)
	if err != nil || out.Estimated != "126" {
		t.Fatal(out, err)
	}
	r.Entries = append(r.Entries, r.Entries[1])
	out, err = ReportTaskCost(r)
	if err != nil || out.Estimated != "126" {
		t.Fatal("modeled duplicate charged twice", out, err)
	}
	r.Entries[1].Nanodollars = "125"
	if _, err := ReportTaskCost(r); err == nil {
		t.Fatal("accepted amount contradicting rates")
	}
	if _, err := priceTaskUnits([]TaskCostUnits{{Kind: "output_tokens", Units: "9223372036854775807", Rate: "9223372036854775807"}}); err == nil {
		t.Fatal("price overflow accepted")
	}
}

package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestHealthReceiptImportPreservesStagesAndMissingMetadata(t *testing.T) {
	for _, scenario := range []struct {
		state  string
		stages []string
		want   string
	}{
		{"prepared_dispatch_owner_active", nil, "unknown_dispatch"},
		{"prepared_without_dispatch", nil, "assembled_unsent"},
		{"outcome_unknown", []string{"dispatch_admitted"}, "unknown_dispatch"},
		{"outcome_unknown", []string{"dispatch_admitted", "dispatch_started"}, "network_uncertain"},
		{"acknowledged", []string{"dispatch_admitted", "acknowledged"}, "dispatched"},
	} {
		t.Run(scenario.want+scenario.state, func(t *testing.T) {
			state := &sourceReleaseState{}
			args := receiptTestAdmission(t, state)
			plan := sourceReleaseCall(t, state, args)
			var prepared providerReceiptEvent
			if err := json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared); err != nil {
				t.Fatal(err)
			}
			now := time.Now().UTC()
			journal, err := newHealthJournal("owner", "operator", "private", "", now.Add(-time.Hour))
			if err != nil {
				t.Fatal(err)
			}
			receipt := inspectedHealthReceipt{Prepared: &prepared, Sequence: "9007199254740993", State: scenario.state, Stages: scenario.stages}
			snapshot, err := healthSnapshotFromReceipt(receipt, journal, 1000000)
			if err != nil {
				t.Fatal(err)
			}
			if snapshot.PreparedSequence != 9007199254740993 || snapshot.Invocation.Task != "" || snapshot.Invocation.Fingerprint != "" || len(snapshot.Invocation.MetadataGaps) == 0 {
				t.Fatal(snapshot)
			}
			if len(snapshot.Invocation.Records) != 1 || snapshot.Invocation.Records[0].Kind != "unknown" || snapshot.Invocation.Records[0].LowTrust != nil {
				t.Fatal(snapshot)
			}
			if err = journal.apply(snapshot, now); err != nil {
				t.Fatal(err)
			}
			if journal.Attempts[prepared.AttemptID].stage() != scenario.want {
				t.Fatal(journal.Attempts)
			}
			// A sampling configuration change must not change an existing attempt's coin.
			next, err := healthSnapshotFromReceipt(receipt, journal, 1)
			if err != nil || next.Invocation.SamplePPM != 1000000 {
				t.Fatal(next, err)
			}
			if err = journal.apply(next, now); err != nil {
				t.Fatal(err)
			}
			pop := healthPopulation{Namespace: "owner", Principal: "operator", Project: "private", Purpose: "provider_input", QueryClass: "unclassified", Stage: scenario.want, From: now.Add(-time.Hour), Until: now.Add(time.Second)}
			report, err := journal.report(pop, now.Add(time.Second))
			if err != nil || report.Attempts[scenario.want] != 1 || report.MetadataGaps["query_fingerprint"] != 1 {
				t.Fatal(report, err)
			}
			public, _ := json.Marshal(report)
			if strings.Contains(string(public), releaseTestRef().ID) || strings.Contains(string(public), "test-turn") {
				t.Fatal("private selection leaked")
			}
			receipt.Sequence = ""
			if _, err = healthSnapshotFromReceipt(receipt, journal, 1000000); err == nil {
				t.Fatal("missing durable sequence accepted")
			}
		})
	}
}

func TestHealthPrivateHostBoundaryAndWindowPlan(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	options := handlerOptions{placement: PlacementServer}
	args := sourceReleaseArgs(map[string]any{"operation": "health-plan", "health_principal": "alice", "window": "2h"})
	if _, code := handleRetrievalHealth(options, bus.ModuleInvocation{PrincipalRef: 1}, args); code != bus.ModuleStatusInvalidRequest {
		t.Fatal(code)
	}
	raw, code := handleRetrievalHealth(options, bus.ModuleInvocation{}, args)
	if code != bus.ModuleStatusOK {
		t.Fatal(code)
	}
	body, err := bus.DecodeCommandResult(raw)
	if err != nil {
		t.Fatal(err)
	}
	var plan struct{ Status, From, Until string }
	if json.Unmarshal(body, &plan) != nil || plan.Status != "ok" {
		t.Fatal(string(body))
	}
	from, _ := time.Parse(time.RFC3339Nano, plan.From)
	until, _ := time.Parse(time.RFC3339Nano, plan.Until)
	if until.Sub(from) != 2*time.Hour {
		t.Fatal(plan)
	}
	for _, window := range []string{"0h", "25h", "invalid", "-1h"} {
		args["window"], _ = json.Marshal(window)
		raw, _ = handleRetrievalHealth(options, bus.ModuleInvocation{}, args)
		body, _ = bus.DecodeCommandResult(raw)
		if !strings.Contains(string(body), "invalid_argument") {
			t.Fatal(string(body))
		}
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "")
	raw, _ = handleRetrievalHealth(options, bus.ModuleInvocation{}, args)
	body, _ = bus.DecodeCommandResult(raw)
	if !strings.Contains(string(body), "disabled") {
		t.Fatal(string(body))
	}
}

func TestHealthTextShowsLossAndUnknownLabels(t *testing.T) {
	journal, p, event := journalFixture(t)
	event.Invocation.MetadataGaps = []string{"family", "query_fingerprint"}
	if err := journal.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	report, err := journal.report(p, p.Until)
	if err != nil {
		t.Fatal(err)
	}
	report.Complete = false
	report.Evicted = 7
	text := healthReportText(report)
	for _, want := range []string{"Window complete: false; capacity evictions: 7", "unknown_dispatch: 1 retained attempts", "Missing family: 1 invocations", "Unmeasured:", "top-1 unknown"} {
		if !strings.Contains(text, want) {
			t.Fatalf("missing %q in %s", want, text)
		}
	}
	for _, private := range []string{event.Invocation.Attempt, event.Invocation.Records[0].RecordID, event.Invocation.Task} {
		if private != "" && strings.Contains(text, private) {
			t.Fatalf("private identity in report: %q", private)
		}
	}
}

func TestHealthTraceReferencesRequireExactScopeAndExplicitSelection(t *testing.T) {
	journal, p, event := journalFixture(t)
	event.Invocation.Request = "private-request"
	event.Acknowledged = true
	if err := journal.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	page := journal.traceReferences(p)
	if len(page.References) != 1 || page.References[0].Request != "private-request" || page.References[0].Stage != "dispatched" {
		t.Fatal(page)
	}
	foreign := p
	foreign.Principal = "another-principal"
	if leaked := journal.traceReferences(foreign); len(leaked.References) != 0 || leaked.MissingRequest != 0 {
		t.Fatal(leaked)
	}
	foreign = p
	foreign.Project = "another-project"
	if leaked := journal.traceReferences(foreign); len(leaked.References) != 0 {
		t.Fatal(leaked)
	}
	// Legacy persisted attempts without a locator remain visibly unavailable.
	row := journal.Attempts[event.Invocation.Attempt]
	row.Invocation.Request = ""
	journal.Attempts[event.Invocation.Attempt] = row
	page = journal.traceReferences(p)
	if len(page.References) != 0 || page.MissingRequest != 1 {
		t.Fatal(page)
	}
}

package memory

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestHealthLabelsPinDenominatorsAndDeduplicateEvidence(t *testing.T) {
	p, events := healthFixture()
	yes, no := true, false
	labels := &healthSelectionLabels{Verifier: "owner-verifier-v1", Candidates: []healthCandidateLabel{
		{ID: "private-a", Arm: "lexical", Population: "candidates", Invalid: &yes},
		{ID: "private-a", Arm: "lexical", Population: "candidates", Invalid: &yes},
		{ID: "private-b", Arm: "lexical", Population: "candidates", Invalid: &no},
		{ID: "private-c", Arm: "lexical", Population: "candidates"},
		{ID: "private-a", Arm: "lexical", Population: "deliveries", Invalid: &no},
	}, Requirements: []healthRequirementLabel{
		{ID: "private-required-a", BaselineArm: "lexical", BaselineSatisfied: &no, FinalSatisfied: &yes, SoleSupportDisplaced: &yes},
		{ID: "private-required-a", BaselineArm: "lexical", BaselineSatisfied: &no, FinalSatisfied: &yes, SoleSupportDisplaced: &yes},
		{ID: "private-required-b", BaselineArm: "lexical", BaselineSatisfied: &no, FinalSatisfied: &no, SoleSupportDisplaced: &no},
		{ID: "private-required-c", BaselineArm: "lexical", BaselineSatisfied: &yes, FinalSatisfied: &yes},
		{ID: "private-required-d", BaselineArm: "lexical"},
	}}
	events[0].Labels = labels
	r, err := aggregateHealth(events, p)
	if err != nil {
		t.Fatal(err)
	}
	if r.Labels.UnlabeledInvocations != 1 {
		t.Fatal(r.Labels)
	}
	contamination := r.Labels.Contamination["candidates:lexical"]
	closeHealth(t, contamination.Rate, 0.5)
	if contamination.Denominator != 2 || contamination.Unknown != 1 {
		t.Fatal(contamination)
	}
	closeHealth(t, r.Labels.Contamination["deliveries:lexical"].Rate, 0)
	recovery := r.Labels.Recovery["lexical"]
	closeHealth(t, recovery.Rate, 0.5)
	if recovery.Denominator != 2 || recovery.Unknown != 1 {
		t.Fatal(recovery)
	}
	closeHealth(t, r.Labels.Displacement.Rate, 0.5)
	if r.Labels.Displacement.Unknown != 2 || len(r.Unsupported) != 0 {
		t.Fatal(r)
	}
	if r.LowTrustFamilyFanout[1] != 1 {
		t.Fatal(r.LowTrustFamilyFanout)
	}
	raw, _ := json.Marshal(r)
	if strings.Contains(string(raw), "private-") {
		t.Fatal("verifier identities leaked")
	}
	labels.Candidates[1].Invalid = &no
	if _, err = aggregateHealth(events, p); err == nil {
		t.Fatal("conflicting duplicate labels accepted")
	}
	labels.Candidates = labels.Candidates[:1]
	labels.Verifier = ""
	if _, err = aggregateHealth(events, p); err == nil {
		t.Fatal("unattributed labels accepted")
	}
}

func TestHealthFamilyFanoutRequiresKnownTrustAndTask(t *testing.T) {
	p, events := healthFixture()
	events[1].Task = "another-private-task"
	r, err := aggregateHealth(events, p)
	if err != nil || r.LowTrustFamilyFanout[2] != 1 {
		t.Fatal(r, err)
	}
	events[1].Task = ""
	r, err = aggregateHealth(events, p)
	if err != nil || r.UnknownFanout != 1 || r.LowTrustFamilyFanout[1] != 1 {
		t.Fatal(r, err)
	}
}

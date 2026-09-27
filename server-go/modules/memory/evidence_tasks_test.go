package memory

import (
	"encoding/json"
	"testing"
)

func temporalCoverageFixture(t *testing.T) (*typedContextResult, typedItem, typedItem) {
	r := coverageFixture(t)
	r.Requirements.QueryMode = "temporal_change"
	r.Channels["historical_assertions"].Enabled = true
	r.Channels["historical_assertions"].Status = "ok"
	old := coverageHit(10, "service", "old-database")
	h := old.value.(assertionHit)
	h.Historical = true
	h.Lifecycle = "superseded"
	h.AssertedAt = "2025-01-01T00:00:00Z"
	h.SupersededAt = "2026-01-01T00:00:00Z"
	h.ValidUntil = h.SupersededAt
	old.value = h
	old.source.ReadPolicy = &sourceReadPolicy{Historical: true}
	current := coverageHit(11, "service", "new-database")
	h = current.value.(assertionHit)
	h.PriorVersionID = "10"
	h.AssertedAt = "2026-01-01T00:00:00Z"
	h.ValidFrom = h.AssertedAt
	current.value = h
	return r, old, current
}
func TestEvidenceTemporalCoherenceAndPacking(t *testing.T) {
	for _, tc := range []struct {
		name   string
		change func(*typedContextResult, *typedItem, *typedItem)
		status string
	}{
		{"canonical transition", func(*typedContextResult, *typedItem, *typedItem) {}, "complete"},
		{"unrelated predecessor", func(_ *typedContextResult, _ *typedItem, c *typedItem) {
			h := c.value.(assertionHit)
			h.PriorVersionID = "999"
			c.value = h
		}, "partial"},
		{"future predecessor", func(_ *typedContextResult, o *typedItem, _ *typedItem) {
			h := o.value.(assertionHit)
			h.AssertedAt = "2027-01-01T00:00:00Z"
			o.value = h
		}, "partial"},
		{"unknown change date", func(_ *typedContextResult, _ *typedItem, c *typedItem) {
			h := c.value.(assertionHit)
			h.ValidFrom = ""
			c.value = h
		}, "partial"},
		{"update dropped", func(r *typedContextResult, _ *typedItem, _ *typedItem) { r.Channels["current_assertions"].Budget = 0 }, "insufficient"},
		{"history unavailable", func(r *typedContextResult, _ *typedItem, _ *typedItem) {
			r.Channels["historical_assertions"].Status = "degraded"
		}, "unknown"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r, o, c := temporalCoverageFixture(t)
			tc.change(r, &o, &c)
			r.add("current_assertions", c)
			r.add("historical_assertions", o)
			if err := r.finish(); err != nil {
				t.Fatal(err)
			}
			if r.Sufficiency != tc.status {
				t.Fatalf("%s: %+v", r.Sufficiency, r.Coverage)
			}
			if tc.name == "update dropped" {
				for _, role := range r.Coverage.Roles {
					if role.Status != "budget_dropped" {
						t.Fatal(role)
					}
				}
			}
			raw, _ := json.Marshal(r)
			copy, err := decodeTypedProjection(string(raw))
			if err != nil {
				t.Fatal(err)
			}
			if err = copy.fitProjectionBytes(len(r.Rendered)); err != nil || copy.Sufficiency != r.Sufficiency {
				t.Fatal(err, copy.Coverage)
			}
		})
	}
}
func TestEvidenceComparisonAndIndependentSupport(t *testing.T) {
	r := coverageFixture(t)
	r.Requirements.QueryMode = "comparison"
	r.Requirements.Obligations = append(r.Requirements.Obligations, evidenceObligation{Subject: "other", Relation: "uses"})
	r.add("current_assertions", coverageHit(1, "service", "db"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "partial" {
		t.Fatal(r.Coverage)
	}
	r.add("current_assertions", coverageHit(2, "other", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "complete" {
		t.Fatal(r.Coverage)
	}
	r.Requirements.Obligations[0].Role = "independent_support"
	r.Requirements.Obligations[0].MinIndependent = 2
	for i := int64(3); i < 33; i++ {
		r.add("current_assertions", coverageHit(i, "service", "db"))
	}
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency == "complete" || r.Coverage.Roles[0].Status != "unavailable" {
		t.Fatal("copies certified independence", r.Coverage)
	}
}

func TestEvidenceProcedureNeedsRetainedConstraint(t *testing.T) {
	r := coverageFixture(t)
	r.Requirements.QueryMode = "procedure_application"
	source := coverageHit(9, "service", "placeholder").source
	source.Kind = "learning_procedure"
	r.add("approved_procedures", typedItem{id: "9", source: source, text: "reviewed procedure", value: map[string]any{"proposal_id": 9, "target_key": "service", "state": "committed", "procedure": map[string]string{"step": "use only approved database"}}})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "partial" {
		t.Fatal(r.Coverage)
	}
	constraint := coverageHit(10, "service", "approved database")
	h := constraint.value.(assertionHit)
	h.Kind = "policy"
	constraint.value = h
	r.add("current_assertions", constraint)
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "complete" {
		t.Fatal(r.Coverage)
	}
	raw, _ := json.Marshal(r)
	copy, err := decodeTypedProjection(string(raw))
	if err != nil {
		t.Fatal(err)
	}
	if err = copy.fitProjectionBytes(0); err != nil || copy.Sufficiency == "complete" {
		t.Fatal(err, copy.Coverage)
	}
}

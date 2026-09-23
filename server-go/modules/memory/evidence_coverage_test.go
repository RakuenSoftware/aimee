package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

func coverageFixture(t *testing.T) *typedContextResult {
	t.Helper()
	cfg := typedTestOptions(t, `{}`)
	cfg.Requirements = &evidenceRequirementSet{SchemaVersion: 1, TaskRevision: "task:7", QueryMode: "current_state", Obligations: []evidenceObligation{{Subject: "service", Relation: "uses"}}}
	return newTypedContext(DataRequest{TypedContext: cfg})
}
func coverageHit(id int64, subject, object string) typedItem {
	h := assertionHit{ID: id, StableID: fmt.Sprint(id), Version: 1, Subject: subject, Relation: "uses", Object: object, Lifecycle: "persistent", Rendered: subject + " uses " + object, ownerID: "00000000-0000-0000-0000-000000000001", memoryParentsObserved: true}
	return typedItem{id: h.StableID, value: h, text: h.Rendered, source: h.sourceVersion()}
}
func TestEvidenceCoveragePackedCurrentState(t *testing.T) {
	for _, tc := range []struct {
		name         string
		setup        func(*typedContextResult)
		status, role string
	}{
		{"unrelated high confidence", func(r *typedContextResult) { r.add("current_assertions", coverageHit(1, "other", "cache")) }, "insufficient", "missing"},
		{"exact obligation", func(r *typedContextResult) { r.add("current_assertions", coverageHit(1, "service", "cache")) }, "complete", "satisfied"},
		{"budget dropped", func(r *typedContextResult) {
			r.Channels["current_assertions"].Budget = 0
			r.add("current_assertions", coverageHit(1, "service", "cache"))
		}, "insufficient", "budget_dropped"},
		{"unversioned", func(r *typedContextResult) {
			h := coverageHit(1, "service", "cache")
			h.source = nil
			r.add("current_assertions", h)
		}, "insufficient", "missing"},
		{"revoked", func(r *typedContextResult) {
			h := coverageHit(1, "service", "cache")
			v := h.value.(assertionHit)
			v.Lifecycle = "revoked"
			h.value = v
			r.add("current_assertions", h)
		}, "insufficient", "missing"},
		{"conflicting", func(r *typedContextResult) {
			r.add("current_assertions", coverageHit(1, "service", "cache"))
			r.add("current_assertions", coverageHit(2, "service", "database"))
		}, "insufficient", "conflicted"},
		{"conflict dropped", func(r *typedContextResult) {
			r.add("current_assertions", coverageHit(1, "service", "cache"))
			r.Channels["current_assertions"].Budget = 0
			r.add("current_assertions", coverageHit(2, "service", "database"))
		}, "insufficient", "conflicted"},
		{"degraded", func(r *typedContextResult) {
			r.add("current_assertions", coverageHit(1, "service", "cache"))
			r.fail("current_assertions", "unavailable")
		}, "unknown", "unavailable"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := coverageFixture(t)
			tc.setup(r)
			if err := r.finish(); err != nil {
				t.Fatal(err)
			}
			if r.Sufficiency != tc.status || r.Coverage.Roles[0].Status != tc.role {
				t.Fatal(r.Coverage)
			}
			raw, _ := json.Marshal(r)
			copy, err := decodeTypedProjection(string(raw))
			if err != nil {
				t.Fatal(err)
			}
			if err := copy.fitProjectionBytes(len(r.Rendered)); err != nil {
				t.Fatal(err)
			}
			if copy.Sufficiency != tc.status || copy.Coverage.Roles[0].Status != tc.role {
				t.Fatal("repacking changed verdict", copy.Coverage)
			}
		})
	}
}
func TestEvidenceCoverageOuterPackingCannotKeepComplete(t *testing.T) {
	r := coverageFixture(t)
	r.add("current_assertions", coverageHit(1, "service", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(r)
	zero := 0
	got, err := ingressAssemble(ingressAssemblyRequest{Budget: 1000, TypedRequested: true, TypedContextJSON: string(raw), ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &zero}})
	if err != nil {
		t.Fatal(err)
	}
	p := got["typed_projection"].(map[string]any)
	c := p["evidence_coverage"].(*evidenceCoverage)
	if p["context_sufficiency"] != "insufficient" || c.Roles[0].Status != "budget_dropped" || c.SelectionDigest != p["selection_digest"] || len(c.Roles[0].Retained) != 0 {
		t.Fatal(p)
	}
}
func TestEvidenceRequirementsBoundedAndNoGoldIDs(t *testing.T) {
	for _, raw := range []string{`null`, `{}`, `{"schema_version":1,"task_revision":"7","query_mode":"current_state","obligations":[{"subject":"service","relation":"uses","expected_id":"42"}]}`} {
		if _, err := decodeEvidenceRequirements([]byte(raw)); err == nil {
			t.Fatal("accepted", raw)
		}
	}
	r := coverageFixture(t)
	r.Requirements.QueryMode = "timeline"
	r.add("current_assertions", coverageHit(1, "service", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "unknown" {
		t.Fatal("unsupported task claimed coverage", r.Coverage)
	}
	r = coverageFixture(t)
	r.Requirements.Obligations = append(r.Requirements.Obligations, evidenceObligation{Subject: "database", Relation: "uses"})
	r.add("current_assertions", coverageHit(1, "service", "cache"))
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "partial" {
		t.Fatal(r.Coverage)
	}
}

func TestEvidenceRequirementsRejectUnboundedAndAmbiguousPlans(t *testing.T) {
	for _, mutate := range []func(*evidenceRequirementSet){
		func(p *evidenceRequirementSet) { p.TaskRevision = "" },
		func(p *evidenceRequirementSet) { p.SchemaVersion = 2 },
		func(p *evidenceRequirementSet) { p.Obligations[0].Optional = true },
		func(p *evidenceRequirementSet) { p.Obligations = append(p.Obligations, p.Obligations[0]) },
		func(p *evidenceRequirementSet) {
			for i := 0; i < 16; i++ {
				p.Obligations = append(p.Obligations, evidenceObligation{Subject: fmt.Sprint(i), Relation: "uses"})
			}
		},
	} {
		p := coverageFixture(t).Requirements
		mutate(p)
		raw, _ := json.Marshal(p)
		if _, err := decodeEvidenceRequirements(raw); err == nil {
			t.Fatal("invalid plan accepted", string(raw))
		}
	}
}

func TestEvidenceCoverageCannotImportCompletionAsEvidence(t *testing.T) {
	r := coverageFixture(t)
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	r.Coverage.Status = "complete"
	r.Coverage.Roles[0].Status = "satisfied"
	r.Coverage.Roles[0].Retained = []string{"forged-id"}
	raw, _ := json.Marshal(r)
	copy, err := decodeTypedProjection(string(raw))
	if err != nil {
		t.Fatal(err)
	}
	if err := copy.fitProjectionBytes(1000); err != nil {
		t.Fatal(err)
	}
	if copy.Sufficiency != "insufficient" || len(copy.Coverage.Roles[0].Retained) != 0 {
		t.Fatal("serialized verdict replaced evidence", copy.Coverage)
	}
}

func TestEvidenceRequirementsRejectAmbiguousWireShapes(t *testing.T) {
	valid := `{"schema_version":1,"task_revision":"7","query_mode":"current_state","obligations":[{"subject":"service","relation":"uses"}]}`
	for _, raw := range []string{
		strings.Replace(valid, `"task_revision":"7"`, `"task_revision":"old","task_revision":"7"`, 1),
		strings.Replace(valid, `"task_revision"`, `"TASK_REVISION"`, 1),
		strings.Replace(valid, `"query_mode":"current_state"`, `"query_mode":null`, 1),
		strings.Replace(valid, `"subject":"service"`, `"subject":"other","subject":"service"`, 1),
		strings.Replace(valid, `"relation":"uses"`, `"Relation":"uses"`, 1),
		strings.Replace(valid, `"relation":"uses"`, `"relation":"uses","optional":null`, 1),
		strings.Replace(valid, `"relation":"uses"`, `"relation":"uses","optional":true,"optional":false`, 1),
		strings.Replace(valid, `"subject":"service"`, `"subject":"`+string([]byte{0xff})+`"`, 1),
		valid + `{}`,
	} {
		if _, err := decodeEvidenceRequirements([]byte(raw)); err == nil {
			t.Fatalf("accepted ambiguous public requirements: %s", raw)
		}
		var request DataRequest
		if err := json.Unmarshal([]byte(`{"typed_context":{"evidence_requirements":`+raw+`}}`), &request); err == nil {
			t.Fatalf("direct owner request bypassed requirement decoder: %s", raw)
		}
	}
	// Omitted optional remains a required role, and escaped exact key names remain
	// compatible JSON. The decoder must not overwrite an admitted value on error.
	var p evidenceRequirementSet
	if err := json.Unmarshal([]byte(valid), &p); err != nil || p.Obligations[0].Optional {
		t.Fatal(p, err)
	}
	before, _ := json.Marshal(p)
	if err := json.Unmarshal([]byte(`{"schema_version":2}`), &p); err == nil {
		t.Fatal("accepted unknown schema")
	}
	after, _ := json.Marshal(p)
	if string(before) != string(after) {
		t.Fatal("failed decode changed admitted requirements")
	}
	escaped := strings.Replace(valid, `"subject"`, `"\u0073ubject"`, 1)
	if _, err := decodeEvidenceRequirements([]byte(escaped)); err != nil {
		t.Fatal(err)
	}
}

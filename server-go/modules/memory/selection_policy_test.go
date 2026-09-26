package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

func TestSelectionRequiredSurvivesThirtyCopies(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_SELECTION_POLICY", typedSelectionPolicyVersion)
	build := func(limit int) *typedContextResult {
		r := coverageFixture(t)
		r.limits = &ContextLimits{SchemaVersion: 1, MaxContextBytes: &limit}
		for i := int64(1); i <= 30; i++ {
			r.add("current_assertions", coverageHit(i, "distractor", "same copy"))
		}
		r.add("current_assertions", coverageHit(31, "service", "required"))
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		return r
	}
	first := build(1600)
	if first.Coverage.Roles[0].Status != "satisfied" || !strings.Contains(first.Rendered, "required") || first.RenderedBytes > 1600 {
		t.Fatal(first.Rendered, first.Coverage)
	}
	if again := build(1600); again.Rendered != first.Rendered || again.SelectionDigest != first.SelectionDigest {
		t.Fatal("nondeterministic")
	}
	for _, limit := range []int{0, 1, 500, 1600} {
		r := build(limit)
		if len(r.Rendered) > limit {
			t.Fatal("floor overrode hard budget")
		}
		if limit < 500 && r.Coverage.Roles[0].Status == "satisfied" {
			t.Fatal("dropped required evidence claimed complete")
		}
	}
	raw, _ := json.Marshal(first)
	imported, err := decodeTypedProjection(string(raw))
	if err != nil {
		t.Fatal(err)
	}
	if err = imported.fitProjectionBytes(1500); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(imported.Rendered, "required") {
		t.Fatal("outer packing lost required reservation")
	}
}
func TestSelectionConstraintCopiesAndCorrection(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_SELECTION_POLICY", typedSelectionPolicyVersion)
	r := coverageFixture(t)
	r.Requirements = nil
	for i := int64(1); i <= 30; i++ {
		item := coverageHit(i, "service", "constraint")
		h := item.value.(assertionHit)
		h.Kind = "policy"
		item.value = h
		r.add("current_assertions", item)
	}
	item := coverageHit(31, "different", "correction")
	h := item.value.(assertionHit)
	h.Authority = 100
	h.PriorVersionID = "old"
	item.value = h
	r.add("current_assertions", item)
	limit := 2200
	r.limits = &ContextLimits{SchemaVersion: 1, MaxContextBytes: &limit}
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(r.Rendered, "constraint") || !strings.Contains(r.Rendered, "correction") {
		t.Fatal(r.Rendered)
	}
}
func TestSelectionDisabledBaselineAndFloors(t *testing.T) {
	build := func(policy string) *typedContextResult {
		t.Setenv("AIMEE_MEMORY_SELECTION_POLICY", policy)
		r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
		for i := 0; i < 70; i++ {
			r.add("observations", typedItem{id: fmt.Sprint(i), text: "same", value: map[string]any{"id": i, "text": "same"}})
		}
		r.add("approved_procedures", typedItem{id: "p", text: "do", value: map[string]string{"procedure": "do"}})
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		return r
	}
	a, b := build(""), build("untrusted-policy")
	if a.Rendered != b.Rendered || a.SelectionPolicy != nil || b.SelectionPolicy != nil {
		t.Fatal("disabled baseline changed")
	}
	active := build(typedSelectionPolicyVersion)
	if len(active.Retained) > 64 || len(active.Channels["approved_procedures"].Items) != 1 {
		t.Fatal("item cap or type reservation")
	}
	if active.SelectionPolicy.Exposure != "disabled" {
		t.Fatal("exposure enabled")
	}
}
func TestSelectionNearRankDiversity(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_SELECTION_POLICY", typedSelectionPolicyVersion)
	r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
	for i, text := range []string{"copy", "copy", "copy", "distinct", "far"} {
		r.add("observations", typedItem{id: fmt.Sprint(i), text: text, value: map[string]string{"text": text}})
	}
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	got := r.Channels["observations"].selected
	if len(got) != 5 || got[1].id != "3" {
		t.Fatal("near-equivalent distinct candidate did not compete", got)
	}
}

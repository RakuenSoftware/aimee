package memory

import (
	"encoding/json"
	"testing"
)

func TestTypedSourceVersionBinding(t *testing.T) {
	const id = "9007199254742999"
	makeProjection := func(owner string) *typedContextResult {
		t.Helper()
		h := assertionHit{ID: 9007199254742999, StableID: id, Version: 7, ownerID: owner,
			Subject: "service", Relation: "uses", Object: "cache", Rendered: "service uses cache"}
		r := newTypedContext(DataRequest{TypedContext: typedTestOptions(t, `{}`)})
		r.add("current_assertions", typedItem{id: id, text: h.Rendered, value: h, source: h.sourceVersion()})
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		return r
	}
	r := makeProjection("00000000-0000-0000-0000-000000000001")
	if len(r.Retained) != 1 || r.SourceVersionState != "record_versions_observed" ||
		r.Retained[0].Source.Version.RecordID != id || r.Retained[0].Source.Version.RecordRevision != "7" {
		t.Fatal("exact owner version missing", r.Retained, r.SourceVersionState)
	}
	rotated := makeProjection("00000000-0000-0000-0000-000000000002")
	if r.Rendered != rotated.Rendered || r.ProjectionDigest != rotated.ProjectionDigest || r.SelectionDigest == rotated.SelectionDigest {
		t.Fatal("owner replacement reused source binding")
	}
	encode := func(r *typedContextResult) string {
		t.Helper()
		raw, err := json.Marshal(r)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	decoded, err := decodeTypedProjection(encode(r))
	if err != nil || decoded.Retained[0].Source.Version != r.Retained[0].Source.Version {
		t.Fatal("opaque host round trip lost source version", decoded, err)
	}
	if err := decoded.fitProjectionBytes(len(r.Rendered)); err != nil || decoded.SelectionDigest != r.SelectionDigest {
		t.Fatal("repacking changed retained source identity", decoded, err)
	}
	if err := decoded.fitProjectionBytes(0); err != nil || len(decoded.Retained) != 0 || decoded.SourceVersionState != "unavailable" {
		t.Fatal("omitted source still claimed as retained", decoded, err)
	}
	for _, mutate := range []func(*typedProjectionRef){
		func(ref *typedProjectionRef) { ref.Source.Version.OwnerID = "00000000-0000-0000-0000-000000000003" },
		func(ref *typedProjectionRef) { ref.Source.Version.RecordRevision = "8" },
		func(ref *typedProjectionRef) { ref.Source.Version.RecordID = "9007199254742998" },
	} {
		probe := makeProjection("00000000-0000-0000-0000-000000000001")
		mutate(&probe.Retained[0])
		if _, err := decodeTypedProjection(encode(probe)); err == nil {
			t.Fatal("changed source accepted under old selection binding")
		}
	}
	// Rehashing metadata cannot reconcile a version with different selected
	// assertion bytes; this checks consistency, not producer authentication.
	r.Retained[0].Source.Version.RecordRevision = "8"
	r.SelectionDigest = typedSelectionDigest(r.ProjectionDigest, r.Retained)
	if _, err := decodeTypedProjection(encode(r)); err == nil {
		t.Fatal("source revision contradicted selected assertion")
	}
	partial := makeProjection("00000000-0000-0000-0000-000000000001")
	partial.add("observations", typedItem{id: "observation", text: "derived", value: map[string]string{"summary": "derived"}})
	if err := partial.finish(); err != nil || partial.SourceVersionState != "partial" {
		t.Fatal("unversioned owner row gained version certainty", partial, err)
	}
}

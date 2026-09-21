package readcontract

import (
	"strings"
	"testing"
)

func TestRestrictionRejectsMalformedAndDuplicateIDs(t *testing.T) {
	valid := SearchRestriction{Version: 1, Namespace: "personal", Authorization: strings.Repeat("a", 64), IDs: []int64{5}}
	if e := valid.Validate(); e != nil {
		t.Fatal(e)
	}
	for _, change := range []func(*SearchRestriction){func(r *SearchRestriction) { r.IDs = nil }, func(r *SearchRestriction) { r.IDs = []int64{5, 5} }, func(r *SearchRestriction) { r.IDs = []int64{0} }, func(r *SearchRestriction) { r.Authorization = strings.Repeat("z", 64) }, func(r *SearchRestriction) { r.Version = 2 }, func(r *SearchRestriction) { r.Namespace = "" }} {
		r := valid
		change(&r)
		if r.Validate() == nil {
			t.Fatalf("accepted %+v", r)
		}
	}
	valid.IDs = []int64{}
	if e := valid.Validate(); e != nil {
		t.Fatal("empty explicit selection should be valid", e)
	}
	var absent *SearchRestriction
	if absent.Validate() == nil {
		t.Fatal("nil accepted")
	}
}

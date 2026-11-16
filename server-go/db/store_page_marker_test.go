package db

import "testing"

// Exercise the actual SQL wire decoder and storeRows.Scan, rather than pgx or
// a family fake that may ignore unsupported destinations.
func TestNullableBooleanHistoryPageMarkerActualStoreCodec(t *testing.T) {
	for _, tc := range []struct {
		name   string
		marker func(*replyBuilder) *replyBuilder
		want   *bool
		bad    bool
	}{
		{"true", func(r *replyBuilder) *replyBuilder { return r.boolean(true) }, boolPageMarker(true), false},
		{"false", func(r *replyBuilder) *replyBuilder { return r.boolean(false) }, boolPageMarker(false), false},
		{"NULL-preserved", func(r *replyBuilder) *replyBuilder { return r.null() }, nil, false},
		{"text-refused", func(r *replyBuilder) *replyBuilder { return r.text("true") }, nil, true},
		{"integer-refused", func(r *replyBuilder) *replyBuilder { return r.num(1) }, nil, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			reply := ok().shape(6, 1).text("1").text("turn").text("owner").text("event").text("payload")
			caller := &fakeCaller{reply: tc.marker(reply).b}
			s := newStore(t, caller)
			rows, err := s.Query(t.Context(), "SELECT exact_history_and_marker")
			if err != nil {
				t.Fatal(err)
			}
			defer rows.Close()
			if !rows.Next() {
				t.Fatal("missing row")
			}
			var a, b, c, d, e string
			var terminal *bool
			err = rows.Scan(&a, &b, &c, &d, &e, &terminal)
			if (err != nil) != tc.bad {
				t.Fatal("wire marker type contract", err)
			}
			if !tc.bad && ((terminal == nil) != (tc.want == nil) || (terminal != nil && *terminal != *tc.want)) {
				t.Fatal("nullable boolean changed")
			}
		})
	}
	caller := &fakeCaller{reply: ok().shape(5, 1).text("1").text("turn").text("owner").text("event").text("payload").b}
	s := newStore(t, caller)
	rows, err := s.Query(t.Context(), "SELECT missing_marker")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	if !rows.Next() {
		t.Fatal("missing row")
	}
	var a, b, c, d, e string
	var terminal *bool
	if err = rows.Scan(&a, &b, &c, &d, &e, &terminal); err == nil {
		t.Fatal("missing marker column accepted")
	}
}
func boolPageMarker(v bool) *bool { return &v }

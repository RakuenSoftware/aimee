package postgres

import "testing"

// The store contract publishes four known answers for the migration checksum,
// in server-go/modules/aimee/store_wire.go, precisely so that each side can be
// checked against the CONTRACT rather than against the other implementation.
//
// Checking the two implementations against each other would agree the day both
// were wrong the same way. These are the stated values.
func TestChecksumMatchesTheContractVectors(t *testing.T) {
	cases := []struct {
		name       string
		statements []string
		want       string
	}{
		{
			name:       "one statement",
			statements: []string{"CREATE TABLE t (a int);"},
			want:       "494759c5f8401e611c3f18d05102716546f00a8272aae8202177f27aac70dcae",
		},
		{
			name:       "two statements",
			statements: []string{"A", "B"},
			want:       "bfae6e09d952d65a7d2bd060a949612d0c4e2c0168dca56bc7485d5058c0d600",
		},
		{
			// The reason the length prefix exists: this must NOT equal the case
			// above. A separator alone would let a split and a join collide, and
			// two different migrations would record the same checksum.
			name:       "the same bytes, joined",
			statements: []string{"AB"},
			want:       "e68e79de268f9d2a92eb58a97f8deb11cb0040701bc469d9879ca16de8d7f898",
		},
		{
			name:       "no statements",
			statements: nil,
			want:       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := checksumOf(c.statements); got != c.want {
				t.Errorf("checksumOf(%q) = %s, contract says %s", c.statements, got, c.want)
			}
		})
	}

	// Stated as its own assertion rather than left implicit in the table: if
	// these two ever agree, the length prefix has stopped working and every
	// re-split migration silently keeps its old checksum.
	if checksumOf([]string{"A", "B"}) == checksumOf([]string{"AB"}) {
		t.Fatal("a split and a join hash the same: the length prefix is not being written")
	}
}

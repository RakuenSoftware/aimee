package aimee

import (
	"bytes"
	"testing"
)

// aimee's binary columns. The JWKS digests are 32 raw bytes each, the nonce
// table's primary key is 32 more, and runtime.content is an unbounded blob.
// Before wireBytes existed the encoder refused a []byte argument outright, so
// those families could not have run at all once aimee talked to the store.

func (r *replyBuilder) blob(b []byte) *replyBuilder {
	r.b = append(r.b, wireBytes)
	r.u32(uint32(len(b)))
	r.b = append(r.b, b...)
	return r
}

func TestRawBytesSurviveTheRoundTrip(t *testing.T) {
	// Deliberately not valid UTF-8: carrying these as text would mean choosing
	// an encoding on one side and guessing it on the other.
	digest := []byte{0x00, 0xff, 0x80, 0x0a, 0x0d, 0x1a, 0xc3, 0x28}
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(StatusOK).str("").str("").
		shape(1, 1).blob(digest).b}

	var got []byte
	err := newStore(t, f).QueryRow(t.Context(),
		"SELECT envelope_sha256 FROM jwks_state WHERE jwks_bytes = $1", digest).Scan(&got)
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	if !bytes.Equal(got, digest) {
		t.Errorf("bytes = % x, want % x", got, digest)
	}
	if !bytes.Contains(f.request, digest) {
		t.Error("the argument did not reach the wire as raw bytes")
	}
}

// TestNullBytesAndEmptyBytesStayApart: NULL and an empty bytea are different
// values, and []byte holds both without the pointer-to-pointer the other types
// need -- a nil slice is distinct from an empty one, so nothing is flattened.
func TestNullBytesAndEmptyBytesStayApart(t *testing.T) {
	for _, tc := range []struct {
		name  string
		build func(*replyBuilder) *replyBuilder
		want  []byte
	}{
		{"null", func(r *replyBuilder) *replyBuilder { return r.null() }, nil},
		{"empty", func(r *replyBuilder) *replyBuilder { return r.blob([]byte{}) }, []byte{}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := &replyBuilder{}
			f := &fakeCaller{reply: tc.build(r.u32(StatusOK).str("").str("").shape(1, 1)).b}
			got := []byte("clobber me")
			if err := newStore(t, f).QueryRow(t.Context(), "SELECT content").Scan(&got); err != nil {
				t.Fatalf("scan: %v", err)
			}
			if (got == nil) != (tc.want == nil) || len(got) != len(tc.want) {
				t.Errorf("got %v (nil=%t), want %v (nil=%t)",
					got, got == nil, tc.want, tc.want == nil)
			}
		})
	}

	// The same distinction going out. Encoding both the same way would make a
	// nullable binary column unwritable in one direction or the other.
	var w frameWriter
	if err := w.value([]byte(nil)); err != nil {
		t.Fatalf("nil []byte: %v", err)
	}
	if w.buf[0] != wireNull {
		t.Errorf("nil []byte encoded as type %d, want NULL", w.buf[0])
	}
	var w2 frameWriter
	if err := w2.value([]byte{}); err != nil {
		t.Fatalf("empty []byte: %v", err)
	}
	if w2.buf[0] != wireBytes {
		t.Errorf("empty []byte encoded as type %d, want bytes", w2.buf[0])
	}
}

// TestBytesAndTextDoNotSubstituteForEachOther: silent conversion between the
// two is how a digest becomes mojibake in a column that then compares unequal
// to itself.
func TestBytesAndTextDoNotSubstituteForEachOther(t *testing.T) {
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(StatusOK).str("").str("").shape(1, 1).blob([]byte{1, 2}).b}
	var s string
	if err := newStore(t, f).QueryRow(t.Context(), "SELECT x").Scan(&s); err == nil {
		t.Error("bytes were accepted into a string destination")
	}

	r2 := &replyBuilder{}
	f2 := &fakeCaller{reply: r2.u32(StatusOK).str("").str("").shape(1, 1).text("hello").b}
	var b []byte
	if err := newStore(t, f2).QueryRow(t.Context(), "SELECT x").Scan(&b); err == nil {
		t.Error("text was accepted into a []byte destination")
	}
}

// TestABlobIsCopiedNotAliased: the frame buffer is reused, so a caller keeping
// the slice would otherwise watch its value change underneath it.
func TestABlobIsCopiedNotAliased(t *testing.T) {
	r := &replyBuilder{}
	f := &fakeCaller{reply: r.u32(StatusOK).str("").str("").shape(1, 1).blob([]byte{7, 8, 9}).b}

	var got []byte
	if err := newStore(t, f).QueryRow(t.Context(), "SELECT x").Scan(&got); err != nil {
		t.Fatalf("scan: %v", err)
	}
	for i := range f.reply {
		f.reply[i] = 0
	}
	if !bytes.Equal(got, []byte{7, 8, 9}) {
		t.Errorf("the scanned blob aliased the frame: % x", got)
	}
}

func TestAnOverLargeBlobArgumentIsRefused(t *testing.T) {
	var w frameWriter
	if err := w.value(make([]byte, MaxCellBytes+1)); err == nil {
		t.Error("a blob over the cell limit was encoded")
	}
}

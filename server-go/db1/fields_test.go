package db1

import (
	"bytes"
	"encoding/binary"
	"errors"
	"testing"
)

// The bytes are the contract, so they are spelled out here rather than produced
// by the encoder and compared to itself. This frame is what
// src/modules/db1/db1_module_api.h describes: op, count, then each field
// length-prefixed.
func TestEncodeFieldsMatchesTheDocumentedFrame(t *testing.T) {
	got, err := EncodeFields(7, []string{"wi-1", "", "42"})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	var want bytes.Buffer
	put := func(v uint32) {
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], v)
		want.Write(b[:])
	}
	put(7) // op
	put(3) // field count
	put(4)
	want.WriteString("wi-1")
	put(0) // an empty field is a field, not an omission
	put(2)
	want.WriteString("42")
	if !bytes.Equal(got, want.Bytes()) {
		t.Fatalf("frame mismatch\n got %x\nwant %x", got, want.Bytes())
	}
}

// The module reads fields as C strings. A NUL would truncate one on the far
// side, and a request that silently means something else is worse than one that
// fails.
func TestEncodeFieldsRefusesNul(t *testing.T) {
	if _, err := EncodeFields(1, []string{"ok", "bad\x00tail"}); !errors.Is(err, ErrNulInField) {
		t.Fatalf("want ErrNulInField, got %v", err)
	}
}

func TestDecodeFieldsRoundTrip(t *testing.T) {
	var frame bytes.Buffer
	put := func(v uint32) {
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], v)
		frame.Write(b[:])
	}
	put(0) // status ok
	put(2)
	put(3)
	frame.WriteString("abc")
	put(0)
	status, fields, err := DecodeFields(frame.Bytes())
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if status != 0 {
		t.Fatalf("status = %d, want 0", status)
	}
	if len(fields) != 2 || fields[0] != "abc" || fields[1] != "" {
		t.Fatalf("fields = %q", fields)
	}
}

// A truncated reply and a short list are the same bytes unless the framing is
// checked, and one of those is data loss the caller would not notice.
func TestDecodeFieldsRefusesTruncation(t *testing.T) {
	var frame bytes.Buffer
	put := func(v uint32) {
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], v)
		frame.Write(b[:])
	}
	put(0)
	put(2)
	put(3)
	frame.WriteString("abc")
	// The second field's header never arrives.
	if _, _, err := DecodeFields(frame.Bytes()); !errors.Is(err, ErrMalformed) {
		t.Fatalf("want ErrMalformed on a truncated frame, got %v", err)
	}
}

// Trailing bytes mean the sender and the reader disagree about the shape, which
// is exactly the case where guessing is worst.
func TestDecodeFieldsRefusesTrailingBytes(t *testing.T) {
	var frame bytes.Buffer
	put := func(v uint32) {
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], v)
		frame.Write(b[:])
	}
	put(0)
	put(1)
	put(1)
	frame.WriteString("a")
	frame.WriteString("unexpected")
	if _, _, err := DecodeFields(frame.Bytes()); !errors.Is(err, ErrMalformed) {
		t.Fatalf("want ErrMalformed on trailing bytes, got %v", err)
	}
}

func TestDecodeFieldsRefusesAbsurdCount(t *testing.T) {
	var frame bytes.Buffer
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], 0)
	frame.Write(b[:])
	binary.LittleEndian.PutUint32(b[:], FieldsMax+1)
	frame.Write(b[:])
	if _, _, err := DecodeFields(frame.Bytes()); !errors.Is(err, ErrMalformed) {
		t.Fatalf("want ErrMalformed on an absurd count, got %v", err)
	}
}

// A list reply carries no row count: the module emits produced*N cells and the
// caller divides. A remainder means the two sides disagree about N, and
// returning a short final row would hand the caller a half-built record.
func TestRowsRefusesARemainder(t *testing.T) {
	if _, err := Rows([]string{"a", "b", "c"}, 2); !errors.Is(err, ErrFieldCount) {
		t.Fatalf("want ErrFieldCount, got %v", err)
	}
	rows, err := Rows([]string{"a", "b", "c", "d"}, 2)
	if err != nil {
		t.Fatalf("rows: %v", err)
	}
	if len(rows) != 2 || rows[1][0] != "c" {
		t.Fatalf("rows = %v", rows)
	}
	empty, err := Rows(nil, 3)
	if err != nil || len(empty) != 0 {
		t.Fatalf("empty list should be zero rows, got %v %v", empty, err)
	}
}

// An unset cell arrives empty and means zero; a malformed one must not be
// rounded into a number, because these carry costs that are compared to caps.
func TestScalarParsing(t *testing.T) {
	if v, err := Atoi(""); err != nil || v != 0 {
		t.Fatalf("empty int cell: %v %v", v, err)
	}
	if v, err := Atof(""); err != nil || v != 0 {
		t.Fatalf("empty double cell: %v %v", v, err)
	}
	if _, err := Atoi("12x"); err == nil {
		t.Fatal("a malformed int cell must not parse")
	}
	if _, err := Atof("1.2.3"); err == nil {
		t.Fatal("a malformed double cell must not parse")
	}
	if got := Ftoa(0.1); got != "0.1" {
		t.Fatalf("Ftoa(0.1) = %q, want an exact round trip", got)
	}
	if v, err := Atof(Ftoa(1.0 / 3.0)); err != nil || v != 1.0/3.0 {
		t.Fatalf("double did not round trip: %v %v", v, err)
	}
}

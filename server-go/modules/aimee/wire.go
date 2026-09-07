// Package aimee is the core server module: the serving side of the store wire,
// shared by every family.
//
// 461 of the module's 463 operations use db1-fields-v2 and all of them answer
// with the same five statuses, so the framing is written once here rather than
// 19 times. server-go/aimee/fields.go is the caller-side peer of these exact
// bytes; the two files are mirrors and a change to either is a change to both.
//
// THE MODULE WAS RENAMED. THE WIRE WAS NOT. This package was `db1`, and became
// `aimee` when the module absorbed everything specific to aimee-server. The
// dialect it speaks is still called db1-fields-v2, the caller-side package is
// still server-go/aimee, and the event kinds its C clients compile against are
// still AIMEE_DB1_EVENT_*. Those are names in a contract with parties that did
// not move -- 461 call sites and a peer module -- so renaming them would be a
// wire break dressed up as tidiness. A name in a contract belongs to the
// contract, not to whoever currently implements it.
//
//	Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//	Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//
// Lengths are little-endian. Scalars travel as decimal text, so the wire has one
// representation per value and neither side negotiates widths or endianness.
package aimee

import (
	"encoding/binary"
	"strconv"
	"strings"
)

// The five result codes every operation answers with, from the catalog's
// result_codes. A family does not get to invent a sixth: callers switch on
// these, and an unknown status is indistinguishable from a corrupt frame.
const (
	StatusOK      uint32 = 0
	StatusMissing uint32 = 1
	StatusInvalid uint32 = 2
	StatusTooLong uint32 = 3
	StatusFailed  uint32 = 4
)

// FieldsMax bounds a decoded request, mirroring the caller-side cap. It is a
// crude second bound: it stops a corrupt count from driving an allocation
// before anything has had a chance to validate it.
const FieldsMax = 1 << 16

// DecodeRequest splits a db1-fields-v2 request into its op and fields.
//
// It refuses any frame whose declared lengths do not account for exactly the
// bytes that arrived. A truncated frame and a short field list are otherwise
// the same bytes, and one of them is a caller silently losing an argument.
func DecodeRequest(frame []byte) (op uint32, fields []string, ok bool) {
	if len(frame) < 8 {
		return 0, nil, false
	}
	op = binary.LittleEndian.Uint32(frame[0:4])
	count := binary.LittleEndian.Uint32(frame[4:8])
	if count > FieldsMax {
		return 0, nil, false
	}
	fields = make([]string, 0, count)
	rest := frame[8:]
	for i := uint32(0); i < count; i++ {
		if len(rest) < 4 {
			return 0, nil, false
		}
		n := binary.LittleEndian.Uint32(rest[:4])
		rest = rest[4:]
		if uint64(n) > uint64(len(rest)) {
			return 0, nil, false
		}
		fields = append(fields, string(rest[:n]))
		rest = rest[n:]
	}
	if len(rest) != 0 {
		return 0, nil, false
	}
	return op, fields, true
}

// EncodeReply builds a db1-fields-v2 response frame.
func EncodeReply(status uint32, fields []string) []byte {
	size := 8
	for _, f := range fields {
		size += 4 + len(f)
	}
	frame := make([]byte, 0, size)
	var scratch [4]byte
	put := func(v uint32) {
		binary.LittleEndian.PutUint32(scratch[:], v)
		frame = append(frame, scratch[:]...)
	}
	put(status)
	put(uint32(len(fields)))
	for _, f := range fields {
		put(uint32(len(f)))
		frame = append(frame, f...)
	}
	return frame
}

// Status is the common case: a reply that carries a result and no fields.
func Status(status uint32) []byte { return EncodeReply(status, nil) }

// ValidField rejects a request field containing NUL. The C module reads fields
// as C strings, so an embedded NUL silently truncates one -- and the store's C
// callers are still on the other end of this wire, so the module must refuse
// what they would misread rather than storing a value they cannot round-trip.
func ValidField(field string) bool { return !strings.ContainsRune(field, 0) }

// ValidFields reports whether every field is free of NUL.
func ValidFields(fields []string) bool {
	for _, f := range fields {
		if !ValidField(f) {
			return false
		}
	}
	return true
}

// Scalars, spelled the way the wire carries them. These mirror the caller-side
// helpers so a value written by one side parses on the other.

// Itoa spells an integer reply cell.
func Itoa(v int) string { return strconv.Itoa(v) }

// I64toa spells a 64-bit integer reply cell.
func I64toa(v int64) string { return strconv.FormatInt(v, 10) }

// Ftoa spells a double reply cell. 'g' with -1 precision round-trips a float64
// exactly, which matters for costs compared against a cap.
func Ftoa(v float64) string { return strconv.FormatFloat(v, 'g', -1, 64) }

// Btoa spells a boolean reply cell as the 0/1 the C side reads.
func Btoa(v bool) string {
	if v {
		return "1"
	}
	return "0"
}

// U64toa spells an UNSIGNED 64-bit reply cell.
//
// It exists because one field on this wire is genuinely unsigned: the guardrail
// family's content_hash is an FNV-1a value the C prints with %llu and parses
// with strtoull, so it runs past 2^63-1 and I64toa would render those values as
// negative numbers the caller would not recognise.
func U64toa(v uint64) string { return strconv.FormatUint(v, 10) }

// Atou64 parses an unsigned 64-bit request cell. An empty cell is zero.
func Atou64(cell string) (uint64, bool) {
	if cell == "" {
		return 0, true
	}
	v, err := strconv.ParseUint(cell, 10, 64)
	return v, err == nil
}

// Atoi parses an integer request cell. An empty cell is zero: a field the
// caller did not set arrives as "", and that is not a parse error.
func Atoi(cell string) (int, bool) {
	if cell == "" {
		return 0, true
	}
	v, err := strconv.Atoi(cell)
	return v, err == nil
}

// Atoi64 parses a 64-bit integer request cell. An empty cell is zero.
func Atoi64(cell string) (int64, bool) {
	if cell == "" {
		return 0, true
	}
	v, err := strconv.ParseInt(cell, 10, 64)
	return v, err == nil
}

// Atof parses a double request cell. An empty cell is zero.
func Atof(cell string) (float64, bool) {
	if cell == "" {
		return 0, true
	}
	v, err := strconv.ParseFloat(cell, 64)
	return v, err == nil
}

// Atob parses a boolean request cell. The C side writes 0/1; anything else is
// a malformed cell rather than a silent false.
func Atob(cell string) (bool, bool) {
	switch cell {
	case "":
		return false, true
	case "0":
		return false, true
	case "1":
		return true, true
	default:
		return false, false
	}
}

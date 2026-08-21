package db1

// The db1-fields-v2 wire, in Go.
//
// The keyed-blob wire in client.go carries the economizer's reducer state and
// nothing else: one key, one payload. Every family after the first is counted
// fields in both directions, because an operation that answers with a row -- or
// with a list of them -- has to have somewhere to put the values.
//
// From src/modules/db1/db1_module_api.h, which is generated from the catalog:
//
//	Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//	Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//
// Lengths are little-endian. Integers travel as decimal text, so the wire has
// one representation for every scalar and neither side has to agree about
// widths or endianness beyond the framing itself.
//
// A list reply carries no row count. The module emits produced*N cells for a
// row of N fields and the caller divides -- see the generated stage code, which
// allocates exactly that. Rows() below is that division, and it refuses a
// remainder rather than returning a short final row.

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// FieldsMax bounds a decoded reply. The module's own replies are capped by
// max_bytes per operation; this is a second, cruder bound so a corrupt length
// cannot make the caller allocate without limit before the count is checked.
const FieldsMax = 1 << 16

var (
	// ErrFieldCount is a reply whose field count is not a whole number of rows
	// for the operation's row width.
	ErrFieldCount = errors.New("db1 reply field count is not a whole number of rows")
	// ErrNulInField is a request field containing NUL. The module reads fields
	// as C strings, so an embedded NUL would silently truncate one.
	ErrNulInField = errors.New("db1 request field contains NUL")
)

// EncodeFields builds a db1-fields-v2 request frame.
func EncodeFields(op uint32, fields []string) ([]byte, error) {
	size := 8
	for _, f := range fields {
		if strings.ContainsRune(f, 0) {
			return nil, ErrNulInField
		}
		size += 4 + len(f)
	}
	frame := make([]byte, 0, size)
	var scratch [4]byte
	put := func(v uint32) {
		binary.LittleEndian.PutUint32(scratch[:], v)
		frame = append(frame, scratch[:]...)
	}
	put(op)
	put(uint32(len(fields)))
	for _, f := range fields {
		put(uint32(len(f)))
		frame = append(frame, f...)
	}
	return frame, nil
}

// DecodeFields splits a response frame into its status and its fields. It
// refuses any frame whose declared lengths do not account for exactly the bytes
// that arrived: a truncated reply and a short list are the same bytes otherwise,
// and one of those is data loss the caller would not notice.
func DecodeFields(response []byte) (uint32, []string, error) {
	if len(response) < 8 {
		return 0, nil, ErrMalformed
	}
	status := binary.LittleEndian.Uint32(response)
	count := binary.LittleEndian.Uint32(response[4:8])
	if count > FieldsMax {
		return 0, nil, ErrMalformed
	}
	fields := make([]string, 0, count)
	rest := response[8:]
	for i := uint32(0); i < count; i++ {
		if len(rest) < 4 {
			return 0, nil, ErrMalformed
		}
		n := binary.LittleEndian.Uint32(rest[:4])
		rest = rest[4:]
		if uint64(n) > uint64(len(rest)) {
			return 0, nil, ErrMalformed
		}
		fields = append(fields, string(rest[:n]))
		rest = rest[n:]
	}
	if len(rest) != 0 {
		return 0, nil, ErrMalformed
	}
	return status, fields, nil
}

// Rows splits a flat list reply into rows of width fields each.
func Rows(fields []string, width int) ([][]string, error) {
	if width <= 0 {
		return nil, fmt.Errorf("db1: row width %d is not positive", width)
	}
	if len(fields)%width != 0 {
		return nil, fmt.Errorf("%w: %d fields, width %d", ErrFieldCount, len(fields), width)
	}
	rows := make([][]string, 0, len(fields)/width)
	for i := 0; i < len(fields); i += width {
		rows = append(rows, fields[i:i+width])
	}
	return rows, nil
}

// Scalars, as the wire carries them: decimal text. The parse helpers below are
// deliberately strict -- a field the module did not set arrives as "", and
// treating that as zero is right, but treating "12x" as 12 is not.

// Itoa is the request-side spelling of an integer field.
func Itoa(v int) string { return strconv.Itoa(v) }

// I64toa is the request-side spelling of a 64-bit integer field.
func I64toa(v int64) string { return strconv.FormatInt(v, 10) }

// Ftoa is the request-side spelling of a double field. 'g' with -1 precision
// round-trips a float64 exactly, which matters for costs that are compared
// against caps.
func Ftoa(v float64) string { return strconv.FormatFloat(v, 'g', -1, 64) }

// Atoi parses an integer reply cell. An empty cell is zero.
func Atoi(cell string) (int, error) {
	if cell == "" {
		return 0, nil
	}
	return strconv.Atoi(cell)
}

// Atoi64 parses a 64-bit integer reply cell. An empty cell is zero.
func Atoi64(cell string) (int64, error) {
	if cell == "" {
		return 0, nil
	}
	return strconv.ParseInt(cell, 10, 64)
}

// Atof parses a double reply cell. An empty cell is zero.
func Atof(cell string) (float64, error) {
	if cell == "" {
		return 0, nil
	}
	return strconv.ParseFloat(cell, 64)
}

// callFields sends a db1-fields-v2 request to the lifecycle stage and returns
// the module's status and reply fields.
//
// Every generated method funnels through here, which is the point: the framing,
// the deadline and the event/stage pair are decided once. A generated method
// that carried its own copy of this would be 45 chances to get the framing
// subtly different.
func (c *Client) callFields(ctx context.Context, op uint32, fields []string) (uint32, []string, error) {
	if c == nil || c.caller == nil {
		return 0, nil, ErrConfig
	}
	frame, err := EncodeFields(op, fields)
	if err != nil {
		return 0, nil, err
	}
	response, err := c.caller.Call(ctx, EventLifecycle, StageLifecycle, 0, c.deadline, frame)
	if err != nil {
		return 0, nil, err
	}
	return DecodeFields(response)
}

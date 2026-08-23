package postgres

import (
	"bytes"
	"encoding/binary"
	"math"
	"testing"
)

// encodeValue mirrors what a client sends, so the tests drive the decoder with
// bytes rather than with a struct it also produced.
func encodeValue(v Value) []byte {
	out := []byte{v.Type}
	switch v.Type {
	case ValueText:
		out = binary.LittleEndian.AppendUint32(out, uint32(len(v.Text)))
		out = append(out, v.Text...)
	case ValueInt:
		out = binary.LittleEndian.AppendUint64(out, uint64(v.Int))
	case ValueFloat:
		out = binary.LittleEndian.AppendUint64(out, math.Float64bits(v.Float))
	case ValueBool:
		if v.Bool {
			out = append(out, 1)
		} else {
			out = append(out, 0)
		}
	case ValueBytes:
		out = binary.LittleEndian.AppendUint32(out, uint32(len(v.Bytes)))
		out = append(out, v.Bytes...)
	case ValueTexts:
		out = binary.LittleEndian.AppendUint32(out, uint32(len(v.Texts)))
		for _, text := range v.Texts {
			out = binary.LittleEndian.AppendUint32(out, uint32(len(text)))
			out = append(out, text...)
		}
	}
	return out
}

func statementRequest(op uint32, id string, handle uint64, sql string, args ...Value) []byte {
	out := binary.LittleEndian.AppendUint32(nil, op)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(id)))
	out = append(out, id...)
	out = binary.LittleEndian.AppendUint64(out, handle)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(sql)))
	out = append(out, sql...)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(args)))
	for _, value := range args {
		out = append(out, encodeValue(value)...)
	}
	return out
}

func TestEveryValueTypeSurvivesTheRoundTrip(t *testing.T) {
	// One type set serves arguments and result cells, so a value that decodes
	// wrong here decodes wrong in both directions.
	args := []Value{
		{Type: ValueNull},
		{Type: ValueText, Text: "hello"},
		{Type: ValueInt, Int: -42},
		{Type: ValueFloat, Float: 1.5},
		{Type: ValueBool, Bool: true},
		{Type: ValueBytes, Bytes: []byte{0x00, 0xff, 0x10}},
		{Type: ValueTexts, Texts: []string{"a", "b"}},
	}
	request, err := DecodeRequest(statementRequest(OpExec, "op_name", 0, "SELECT 1", args...))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(request.Args) != len(args) {
		t.Fatalf("args = %d, want %d", len(request.Args), len(args))
	}
	if request.Args[1].Text != "hello" || request.Args[2].Int != -42 ||
		request.Args[3].Float != 1.5 || !request.Args[4].Bool ||
		!bytes.Equal(request.Args[5].Bytes, []byte{0x00, 0xff, 0x10}) ||
		len(request.Args[6].Texts) != 2 {
		t.Fatalf("round trip lost a value: %+v", request.Args)
	}
	if request.StatementID != "op_name" || request.SQL != "SELECT 1" {
		t.Fatalf("statement = %q %q", request.StatementID, request.SQL)
	}
}

func TestNullAndEmptyAreDifferentValues(t *testing.T) {
	// The distinction the whole type set exists to preserve. A column set to
	// NULL and a column set to empty are different facts, and a wire that
	// flattens them makes the difference unrecoverable at the far end.
	request, err := DecodeRequest(statementRequest(OpExec, "op", 0, "SELECT 1",
		Value{Type: ValueNull},
		Value{Type: ValueText, Text: ""},
		Value{Type: ValueBytes, Bytes: []byte{}}))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if request.Args[0].Type != ValueNull {
		t.Error("null decoded as something else")
	}
	if request.Args[1].Type != ValueText {
		t.Error("an empty string decoded as null")
	}
	if request.Args[2].Type != ValueBytes || request.Args[2].Bytes == nil {
		t.Error("an empty byte slice decoded as null; a nullable binary column " +
			"becomes unwritable in one direction")
	}

	// And the same distinction at the binding layer: null must bind as nil,
	// not as a zero of the column's type.
	bound, err := bind(Value{Type: ValueNull})
	if err != nil || bound != nil {
		t.Errorf("null bound as %#v", bound)
	}
	bound, err = bind(Value{Type: ValueText, Text: ""})
	if err != nil || bound != "" {
		t.Errorf("empty text bound as %#v", bound)
	}
}

func TestTrailingBytesAreRefused(t *testing.T) {
	// Extra bytes mean the sender and this disagree about the frame. Ignoring
	// them would let a version skew read as a valid call and run a statement
	// with arguments nobody agreed on.
	body := append(statementRequest(OpExec, "op", 0, "SELECT 1"), 0xff)
	if _, err := DecodeRequest(body); err == nil {
		t.Fatal("a frame with trailing bytes decoded")
	}
}

func TestOversizedFieldsAreRefusedNotTruncated(t *testing.T) {
	// A limit the sender checks is a convention; this is the control. Truncating
	// instead would run a statement that is not the one the caller wrote.
	out := binary.LittleEndian.AppendUint32(nil, OpExec)
	out = binary.LittleEndian.AppendUint32(out, uint32(MaxStatementIDBytes+1))
	out = append(out, bytes.Repeat([]byte("x"), MaxStatementIDBytes+1)...)
	if _, err := DecodeRequest(out); err == nil {
		t.Fatal("an oversized statement_id was accepted")
	}
}

func TestABoolIsZeroOrOne(t *testing.T) {
	// Anything else is a sender that thinks the field means something it does
	// not, and guessing which way it meant is how a wrong answer becomes data.
	out := binary.LittleEndian.AppendUint32(nil, OpExec)
	out = binary.LittleEndian.AppendUint32(out, 2)
	out = append(out, "op"...)
	out = binary.LittleEndian.AppendUint64(out, 0)
	out = binary.LittleEndian.AppendUint32(out, 1)
	out = append(out, "x"...)
	out = binary.LittleEndian.AppendUint32(out, 1)
	out = append(out, ValueBool, 7)
	if _, err := DecodeRequest(out); err == nil {
		t.Fatal("a bool of 7 was accepted")
	}
}

func TestUnknownOperationIsRefused(t *testing.T) {
	body := binary.LittleEndian.AppendUint32(nil, 99)
	if _, err := DecodeRequest(body); err == nil {
		t.Fatal("an unknown operation decoded")
	}
}

func TestMigrateCarriesItsOwnerVersionAndStatements(t *testing.T) {
	out := binary.LittleEndian.AppendUint32(nil, OpMigrate)
	out = binary.LittleEndian.AppendUint32(out, 3)
	out = append(out, "db1"...)
	out = binary.LittleEndian.AppendUint64(out, 7)
	out = binary.LittleEndian.AppendUint32(out, 4)
	out = append(out, "abcd"...)
	out = binary.LittleEndian.AppendUint32(out, 2)
	for _, statement := range []string{"CREATE TABLE a (id BIGINT)", "CREATE INDEX ON a (id)"} {
		out = binary.LittleEndian.AppendUint32(out, uint32(len(statement)))
		out = append(out, statement...)
	}
	request, err := DecodeRequest(out)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if request.Owner != "db1" || request.Version != 7 || request.Checksum != "abcd" ||
		len(request.Statements) != 2 {
		t.Fatalf("migrate = %+v", request)
	}
}

func TestReplyHeaderCarriesSQLStateOnFailure(t *testing.T) {
	// A caller retries a serialization failure, treats a unique violation as an
	// expected answer, and escalates an outage. It cannot do any of that from a
	// status alone.
	body := EncodeError(StatusStatementFailed, "23505", "duplicate key")
	if binary.LittleEndian.Uint32(body[0:4]) != StatusStatementFailed {
		t.Fatal("status missing")
	}
	length := binary.LittleEndian.Uint32(body[4:8])
	if string(body[8:8+length]) != "23505" {
		t.Fatalf("sqlstate = %q", string(body[8:8+length]))
	}
}

func TestQueryReplyCarriesWidthBeforeRows(t *testing.T) {
	// Width first so a reader can size a row before reading one, rather than
	// inferring it from the first row and being wrong about an empty result.
	body := EncodeQueryReply(2, nil)
	at := 4 + 4 + 4 // status, empty sqlstate, empty message
	if binary.LittleEndian.Uint32(body[at:at+4]) != 2 {
		t.Fatal("width missing from an empty result set")
	}
	if binary.LittleEndian.Uint32(body[at+4:at+8]) != 0 {
		t.Fatal("row count wrong")
	}
}

func TestStatusIntegersAreTheContract(t *testing.T) {
	// These integers cross the wire. A peer built against one numbering and a
	// module built against another both compile, both pass their own tests, and
	// the disagreement shows up as a caller acting on the wrong fact: reading
	// "unsupported" as "limit exceeded" and retrying forever, or reading a
	// failure as OK.
	//
	// They are declared explicitly rather than by iota for that reason -- with
	// iota, inserting a status in the middle silently renumbers every one after
	// it. This test is the second guard, for the edit that renumbers a constant
	// directly rather than by insertion. Neither catches what the other does.
	for _, c := range []struct {
		name  string
		value uint32
		want  uint32
	}{
		{"OK", StatusOK, 0},
		{"InvalidRequest", StatusInvalidRequest, 1},
		{"Unsupported", StatusUnsupported, 2},
		{"LimitExceeded", StatusLimitExceeded, 3},
		{"StatementFailed", StatusStatementFailed, 4},
		{"Unavailable", StatusUnavailable, 5},
		{"MigrationFailed", StatusMigrationFailed, 6},
	} {
		if c.value != c.want {
			t.Errorf("Status%s = %d, and the wire says %d", c.name, c.value, c.want)
		}
	}
}

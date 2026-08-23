package postgres

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"strings"
	"syscall"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
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
	pinned := 0
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
		pinned++
	}
	// The third guard. Explicit values stop renumbering-by-insertion and the
	// loop above stops a direct renumber, but neither notices a status added
	// with a number of its own that simply never joined the list -- which is how
	// the far end of this wire came to have two statuses in use and seventeen of
	// nineteen checked.
	if pinned != statusCount {
		t.Errorf("pinned %d statuses, the package declares %d; one is crossing the "+
			"wire unchecked", pinned, statusCount)
	}
}

func TestAFailureIsClassifiedByWhatActuallyHappened(t *testing.T) {
	// Each case is a fact a caller acts on differently. Before these were
	// separated, the last three all arrived as StatusStatementFailed with an
	// empty SQLSTATE -- "your statement is wrong" for a statement PostgreSQL
	// never read.
	for _, c := range []struct {
		name       string
		err        error
		wantStatus uint32
		wantState  string
	}{
		{
			// The server read the statement and refused it. The only case where
			// rewriting the statement is the repair.
			name:       "the server rejected it",
			err:        &pgconn.PgError{Code: "23505", Message: "duplicate key"},
			wantStatus: StatusStatementFailed,
			wantState:  "23505",
		},
		{
			// Wrapped, because a driver error rarely arrives bare.
			name:       "wrapped rejection",
			err:        fmt.Errorf("exec: %w", &pgconn.PgError{Code: "23503"}),
			wantStatus: StatusStatementFailed,
			wantState:  "23503",
		},
		{
			name:       "the caller's deadline expired",
			err:        context.DeadlineExceeded,
			wantStatus: StatusStatementFailed,
			wantState:  sqlStateQueryCanceled,
		},
		{
			name:       "the call was cancelled",
			err:        fmt.Errorf("query: %w", context.Canceled),
			wantStatus: StatusStatementFailed,
			wantState:  sqlStateQueryCanceled,
		},
		{
			name:       "the connection was closed under us",
			err:        fmt.Errorf("write: %w", net.ErrClosed),
			wantStatus: StatusUnavailable,
			wantState:  sqlStateConnectionFailure,
		},
		{
			name:       "the peer reset it",
			err:        fmt.Errorf("read: %w", syscall.ECONNRESET),
			wantStatus: StatusUnavailable,
			wantState:  sqlStateConnectionFailure,
		},
		{
			name:       "the stream ended mid-message",
			err:        io.ErrUnexpectedEOF,
			wantStatus: StatusUnavailable,
			wantState:  sqlStateConnectionFailure,
		},
		{
			// Deliberately unchanged. Guessing "unavailable" for an error we
			// cannot name would be the same collapse pointing the other way, so
			// an unfamiliar client-side error keeps the status it had and says
			// nothing it cannot support.
			name:       "something we cannot name",
			err:        errors.New("pgx: could not encode argument"),
			wantStatus: StatusStatementFailed,
			wantState:  "",
		},
	} {
		t.Run(c.name, func(t *testing.T) {
			status, state, detail := classifyFailure(c.err)
			if status != c.wantStatus || state != c.wantState {
				t.Errorf("status = %d/%q, want %d/%q", status, state, c.wantStatus, c.wantState)
			}
			if detail == "" {
				t.Error("the detail was dropped; the caller is left with a number")
			}
		})
	}
}

func TestALengthPastTheFieldIsRefusedNotWrapped(t *testing.T) {
	// The conversion does not fail, it wraps -- and on a length-prefixed wire a
	// wrapped length is not a wrong number. A value of 2^32+8 bytes writes the
	// length 0 and then appends all of them, so the reader takes the next four
	// bytes of CONTENT as the next length prefix. Everything after that point is
	// framing the sender chose.
	//
	// Nothing reaches this today: statements and cells stop at 1 MiB, rows and
	// arguments at 4096, a PostgreSQL value at 1 GB. Every one of those bounds
	// was added by somebody, and the encoder outlives all of them.
	//
	// Tested here rather than through EncodeQueryReply because the honest
	// end-to-end version needs a four-gigabyte string. A check whose test cannot
	// be written is a check that does not get written.
	for _, c := range []struct {
		name    string
		length  int
		refused bool
	}{
		{"empty", 0, false},
		{"ordinary", 4096, false},
		{"the largest that fits", math.MaxUint32, false},
		{"one past the field", math.MaxUint32 + 1, true},
		{"far past it", math.MaxUint32 * 3, true},
		{"negative", -1, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			w := &writer{}
			w.u32len(c.length)
			if refused := w.err != nil; refused != c.refused {
				t.Fatalf("length %d: refused = %v, want %v (err = %v)",
					c.length, refused, c.refused, w.err)
			}
			// Four bytes either way: a refusal must not also desynchronise the
			// buffer it was recording a problem about.
			if len(w.buf) != 4 {
				t.Fatalf("wrote %d bytes for a length field", len(w.buf))
			}
		})
	}

	// And a recorded refusal becomes a status the caller understands rather than
	// a message whose structure the payload picked.
	w := &writer{}
	w.u32len(math.MaxUint32 + 1)
	framed := w.frame()
	if status := binary.LittleEndian.Uint32(framed[0:4]); status != StatusLimitExceeded {
		t.Fatalf("a reply that could not be framed answered status %d", status)
	}
}

func TestNoLengthBypassesTheFieldCheck(t *testing.T) {
	// u32len ranges over the lengths that go through u32len, which is a
	// population every future call site decides to join. A new field written as
	// w.u32(uint32(len(x))) skips the check, and nothing anywhere says so --
	// the same enumeration bug as a gate that lists its packages.
	//
	// Structural enforcement is not available here: Go has no way to make the
	// unchecked spelling unavailable inside a package. A completeness check is
	// what is left, and it fails on the edit rather than on the incident.
	for _, name := range []string{"sql_wire.go", "sql.go", "sql_tx.go", "schema.go"} {
		source, err := os.ReadFile(name)
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		for number, line := range strings.Split(string(source), "\n") {
			if strings.Contains(line, "uint32(len(") {
				t.Errorf("%s:%d writes a length without the field check:\n\t%s\n"+
					"use w.u32len(len(x)) -- a wrapped length prefix turns the "+
					"payload into framing", name, number+1, strings.TrimSpace(line))
			}
		}
	}
}

// shapedRows answers with whatever shape a test asks for, so the reply ceilings
// can be reached without a database that has four thousand columns.
type shapedRows struct {
	columns int
	rows    int
	at      int
}

func (r *shapedRows) Next() bool {
	r.at++
	return r.at <= r.rows
}

func (r *shapedRows) Values() ([]any, error) {
	out := make([]any, 0, r.columns)
	for index := 0; index < r.columns; index++ {
		out = append(out, int64(index))
	}
	return out, nil
}

func (r *shapedRows) FieldDescriptions() []pgconn.FieldDescription {
	return make([]pgconn.FieldDescription, r.columns)
}

func (r *shapedRows) Err() error                    { return nil }
func (r *shapedRows) Close()                        {}
func (r *shapedRows) CommandTag() pgconn.CommandTag { return pgconn.CommandTag{} }
func (r *shapedRows) RawValues() [][]byte           { return nil }
func (r *shapedRows) Conn() *pgx.Conn               { return nil }
func (r *shapedRows) Scan(...any) error             { return nil }

func TestTheReplyCeilingsRefuseRatherThanTruncate(t *testing.T) {
	// Both refusals existed and nothing had ever made either of them fire.
	//
	// Truncating instead would be worse than failing in both cases. A caller
	// handed exactly the ceiling cannot tell a complete answer from a capped one
	// and will record the cap as fact. And a width that wrapped would tell the
	// reader to size EVERY row wrong, which is a misparse of the whole reply
	// rather than one bad number.
	for _, c := range []struct {
		name    string
		columns int
		rows    int
		refused bool
	}{
		{"an ordinary result", 3, 10, false},
		{"exactly the row ceiling", 2, MaxReplyRows, false},
		{"one row past it", 2, MaxReplyRows + 1, true},
		{"exactly the column ceiling", MaxReplyColumns, 1, false},
		{"one column past it", MaxReplyColumns + 1, 1, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			body := collectReply(&shapedRows{columns: c.columns, rows: c.rows})
			status := binary.LittleEndian.Uint32(body[0:4])
			if refused := status == StatusLimitExceeded; refused != c.refused {
				t.Fatalf("status = %d, refused = %v, want %v", status, refused, c.refused)
			}
		})
	}
}

func TestAnArgumentCountPastTheCeilingIsRefused(t *testing.T) {
	// Refused from the header field before a single argument is read, so a
	// sender claiming five thousand arguments costs nothing to reject. That is
	// also why it is cheap to test and had never been tested: it needs a crafted
	// count rather than four thousand arguments.
	out := binary.LittleEndian.AppendUint32(nil, OpExec)
	out = binary.LittleEndian.AppendUint32(out, 2)
	out = append(out, "op"...)
	out = binary.LittleEndian.AppendUint64(out, 0)
	out = binary.LittleEndian.AppendUint32(out, 1)
	out = append(out, "x"...)
	out = binary.LittleEndian.AppendUint32(out, uint32(MaxArguments+1))
	if _, err := DecodeRequest(out); err == nil {
		t.Fatal("a request claiming more arguments than the ceiling decoded")
	}
}

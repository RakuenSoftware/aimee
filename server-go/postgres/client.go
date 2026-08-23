// Package postgres is the caller side of the postgres module's storage wire.
//
// The module serves storage; this is how a module that owns rows reaches it.
// It lives beside server-go/db1, server-go/db2 and server-go/db3 for the same
// reason those do: a caller needs the contract without importing the
// implementation, and a module that imported the postgres module would be
// linking a database into a process whose whole point is not to have one.
//
// One codec, not one per caller. Every module that holds rows -- control-plane,
// aimee, db1 -- speaks this, and a second encoding of the same wire is how two
// halves of one contract drift apart while both look correct.
package postgres

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// Operation codes and value types, matching the served contract exactly. They
// are the wire's numbers rather than an internal enum: renaming a constant
// changes nothing, changing a number changes what the far end believes.
const (
	opExec           uint32 = 1
	opQuery          uint32 = 2
	opBegin          uint32 = 3
	opCommit         uint32 = 4
	opRollback       uint32 = 5
	opMigrate        uint32 = 6
	opCurrentVersion uint32 = 7
)

const (
	typeNull  uint8 = 0
	typeText  uint8 = 1
	typeInt   uint8 = 2
	typeFloat uint8 = 3
	typeBool  uint8 = 4
	typeTexts uint8 = 5
	typeBytes uint8 = 6
)

// Value.Type is exported and its values were not, which left a caller wanting
// to know what a cell holds comparing against a literal. Scanning has to ask:
// writing a float destination from a text cell yields a zero that reads as
// data, so the check needs a name for what it is checking.
const (
	TypeNull  = typeNull
	TypeText  = typeText
	TypeInt   = typeInt
	TypeFloat = typeFloat
	TypeBool  = typeBool
	TypeTexts = typeTexts
	TypeBytes = typeBytes
)

// Statuses the module answers with.
const (
	statusOK              uint32 = 0
	statusInvalidRequest  uint32 = 1
	statusUnsupported     uint32 = 2
	statusLimitExceeded   uint32 = 3
	statusStatementFailed uint32 = 4
	statusUnavailable     uint32 = 5
	statusMigrationFailed uint32 = 6

	// See the module side: the count is what makes the pinned list complete
	// rather than merely correct. Bump it in the same edit that adds a status.
	statusCount = 7
)

// Limits checked before sending. The module enforces its own copy -- a limit
// the sender checks is a convention, not a control -- but failing here gives a
// better error and costs no round trip.
const (
	MaxStatementBytes   = 1 << 20
	MaxArguments        = 4096
	MaxCellBytes        = 1 << 20
	MaxStatementIDBytes = 64
)

// Error carries what the module said, so a caller can act on the SQLSTATE
// rather than on a string.
//
// The distinction that matters most: 25P01 means the transaction is gone and
// everything in it with it, where an ordinary constraint failure leaves the
// transaction live with one statement wrong. A caller that collapses them logs
// "that insert failed, carrying on" and then commits nothing.
type Error struct {
	Status   uint32
	SQLState string
	Message  string
}

func (e *Error) Error() string {
	if e.SQLState == "" {
		return fmt.Sprintf("postgres: status %d: %s", e.Status, e.Message)
	}
	return fmt.Sprintf("postgres: %s: %s", e.SQLState, e.Message)
}

// IsTransactionClosed reports the handle no longer names a live transaction --
// expired, reclaimed, or never the caller's. Everything written through it is
// gone and the work has to start over.
func IsTransactionClosed(err error) bool {
	var storeErr *Error
	return errors.As(err, &storeErr) &&
		(storeErr.SQLState == "25P01" || storeErr.SQLState == "25P02")
}

// IsUniqueViolation reports a duplicate key, which is often an expected answer
// rather than a failure.
func IsUniqueViolation(err error) bool { return hasState(err, "23505") }

// IsForeignKeyViolation reports a reference to a row that is not there, which
// is a caller error rather than an outage.
func IsForeignKeyViolation(err error) bool { return hasState(err, "23503") }

// IsCheckViolation reports a value the schema refuses.
func IsCheckViolation(err error) bool { return hasState(err, "23514") }

// IsNotNullViolation reports a missing required value.
func IsNotNullViolation(err error) bool { return hasState(err, "23502") }

// IsUnavailable reports that the statement never reached PostgreSQL.
//
// This is the one failure worth retrying unchanged. Every other error above
// says the server read the statement and objected to something in it, so
// sending it again gets the same objection; this one says the server never saw
// it. A caller that cannot tell them apart either retries a bad statement
// forever or gives up on a healthy one because a connection blinked.
func IsUnavailable(err error) bool {
	var storeErr *Error
	return errors.As(err, &storeErr) && storeErr.Status == statusUnavailable
}

// IsPermanent reports that the same request will never succeed.
//
// InvalidRequest, Unsupported and LimitExceeded are all decisions about the
// request rather than about the moment: a statement over the size ceiling, a
// column type this wire has no representation for, a result larger than the bus
// will carry. Sending it again produces the same answer.
//
// It exists because IsUnavailable's promise is only usable if its opposite can
// be named. "The one failure worth retrying unchanged" tells a caller what to
// retry and nothing about what to stop retrying, and a retry loop keyed on "an
// error I do not recognise" will sit on a 2 MiB statement forever.
//
// A statement PostgreSQL refused is deliberately not here. A unique violation
// may become insertable, a foreign key may arrive: those are facts about the
// data at a moment, and the SQLSTATE predicates above are how a caller reads
// them.
func IsPermanent(err error) bool {
	var storeErr *Error
	if !errors.As(err, &storeErr) {
		return false
	}
	switch storeErr.Status {
	case statusInvalidRequest, statusUnsupported, statusLimitExceeded:
		return true
	}
	return false
}

// IsCanceled reports that the caller's own deadline ended the statement.
//
// Distinct from unavailable: the database is fine and the statement may well be
// fine, but the time allowed for it ran out. The repair is a longer deadline or
// a cheaper query, not a retry of the same call with the same budget.
func IsCanceled(err error) bool { return hasState(err, "57014") }

func hasState(err error, state string) bool {
	var storeErr *Error
	return errors.As(err, &storeErr) && storeErr.SQLState == state
}

// Value is one argument or result cell.
//
// Null is its own type rather than a zero. A column set to NULL and a column
// set to empty are different facts, and flattening them makes the difference
// unrecoverable -- which is why Bytes distinguishes nil from an empty slice
// natively, and why scanning NULL into a destination that cannot hold it is an
// error rather than a zero.
type Value struct {
	Type  uint8
	Text  string
	Int   int64
	Float float64
	Bool  bool
	Texts []string
	Bytes []byte
}

// Null, Text, Int, Float, Bool, Bytes and Texts build arguments.
func Null() Value            { return Value{Type: typeNull} }
func Text(v string) Value    { return Value{Type: typeText, Text: v} }
func Int(v int64) Value      { return Value{Type: typeInt, Int: v} }
func Float(v float64) Value  { return Value{Type: typeFloat, Float: v} }
func Bool(v bool) Value      { return Value{Type: typeBool, Bool: v} }
func Texts(v []string) Value { return Value{Type: typeTexts, Texts: v} }

// Bytes carries binary. A nil slice is NULL; a non-nil empty slice is an empty
// value, and the two round-trip distinctly.
func Bytes(v []byte) Value {
	if v == nil {
		return Value{Type: typeNull}
	}
	return Value{Type: typeBytes, Bytes: v}
}

// Caller sends one encoded request and returns the reply body. Production binds
// it to the event bus; a test binds it to the handler directly.
type Caller func(ctx context.Context, body []byte) ([]byte, error)

// Client speaks the storage wire on behalf of one module.
//
// StatementID is not passed per call. It is the caller's OPERATION name, set
// once where the operation is dispatched, so the module can say what a caller
// was doing without parsing SQL and no call site has to remember to pass it.
type Client struct {
	call Caller
}

func New(call Caller) *Client { return &Client{call: call} }

type writer struct {
	buf []byte
	err error
}

func (w *writer) u32(v uint32) { w.buf = binary.LittleEndian.AppendUint32(w.buf, v) }
func (w *writer) u64(v uint64) { w.buf = binary.LittleEndian.AppendUint64(w.buf, v) }

// u32len narrows a length into the wire's four-byte field, or records a
// refusal. See the module side: the conversion wraps rather than failing, and a
// wrapped length on a length-prefixed wire turns content into framing.
func (w *writer) u32len(n int) {
	if n < 0 || int64(n) > math.MaxUint32 {
		w.err = fmt.Errorf("postgres: one past the field: u32(%d) = %d", n, uint32(n))
		w.u32(0)
		return
	}
	w.u32(uint32(n))
}

func (w *writer) str(v string) { w.u32len(len(v)); w.buf = append(w.buf, v...) }

func (w *writer) value(v Value) error {
	w.buf = append(w.buf, v.Type)
	switch v.Type {
	case typeNull:
	case typeText:
		if len(v.Text) > MaxCellBytes {
			return fmt.Errorf("postgres: text cell is %d bytes, over %d",
				len(v.Text), MaxCellBytes)
		}
		w.str(v.Text)
	case typeInt:
		w.u64(uint64(v.Int))
	case typeFloat:
		w.u64(math.Float64bits(v.Float))
	case typeBool:
		if v.Bool {
			w.buf = append(w.buf, 1)
		} else {
			w.buf = append(w.buf, 0)
		}
	case typeBytes:
		if len(v.Bytes) > MaxCellBytes {
			return fmt.Errorf("postgres: byte cell is %d bytes, over %d",
				len(v.Bytes), MaxCellBytes)
		}
		w.u32len(len(v.Bytes))
		w.buf = append(w.buf, v.Bytes...)
	case typeTexts:
		w.u32len(len(v.Texts))
		for _, text := range v.Texts {
			w.str(text)
		}
	default:
		return fmt.Errorf("postgres: unknown value type %d", v.Type)
	}
	return nil
}

func statement(op uint32, statementID string, handle uint64, sql string,
	args []Value) ([]byte, error) {
	if statementID == "" {
		return nil, errors.New("postgres: statement_id is required")
	}
	if len(statementID) > MaxStatementIDBytes {
		return nil, fmt.Errorf("postgres: statement_id is %d bytes, over %d",
			len(statementID), MaxStatementIDBytes)
	}
	if len(sql) > MaxStatementBytes {
		return nil, fmt.Errorf("postgres: statement is %d bytes, over %d",
			len(sql), MaxStatementBytes)
	}
	if len(args) > MaxArguments {
		return nil, fmt.Errorf("postgres: %d arguments, over %d",
			len(args), MaxArguments)
	}
	w := &writer{}
	w.u32(op)
	w.str(statementID)
	w.u64(handle)
	w.str(sql)
	w.u32len(len(args))
	for _, arg := range args {
		if err := w.value(arg); err != nil {
			return nil, err
		}
	}
	if w.err != nil {
		return nil, w.err
	}
	return w.buf, nil
}

type reader struct {
	buf []byte
	at  int
}

func (r *reader) u32() (uint32, error) {
	if r.at+4 > len(r.buf) {
		return 0, errors.New("postgres: truncated reply")
	}
	v := binary.LittleEndian.Uint32(r.buf[r.at:])
	r.at += 4
	return v, nil
}

func (r *reader) u64() (uint64, error) {
	if r.at+8 > len(r.buf) {
		return 0, errors.New("postgres: truncated reply")
	}
	v := binary.LittleEndian.Uint64(r.buf[r.at:])
	r.at += 8
	return v, nil
}

func (r *reader) str() (string, error) {
	length, err := r.u32()
	if err != nil {
		return "", err
	}
	// Summed in the wider type on both sides. This is correct today because int
	// is 64 bits here, which is a fact about the platform rather than about the
	// arithmetic -- and a bound that is right for a reason the reader cannot see
	// is one an edit can quietly take away.
	if int64(r.at)+int64(length) > int64(len(r.buf)) {
		return "", errors.New("postgres: truncated reply")
	}
	v := string(r.buf[r.at : r.at+int(length)])
	r.at += int(length)
	return v, nil
}

func (r *reader) value() (Value, error) {
	if r.at >= len(r.buf) {
		return Value{}, errors.New("postgres: truncated reply")
	}
	kind := r.buf[r.at]
	r.at++
	switch kind {
	case typeNull:
		return Value{Type: kind}, nil
	case typeText:
		text, err := r.str()
		return Value{Type: kind, Text: text}, err
	case typeInt:
		raw, err := r.u64()
		return Value{Type: kind, Int: int64(raw)}, err
	case typeFloat:
		raw, err := r.u64()
		return Value{Type: kind, Float: math.Float64frombits(raw)}, err
	case typeBool:
		if r.at >= len(r.buf) {
			return Value{}, errors.New("postgres: truncated reply")
		}
		raw := r.buf[r.at]
		r.at++
		return Value{Type: kind, Bool: raw == 1}, nil
	case typeBytes:
		length, err := r.u32()
		if err != nil {
			return Value{}, err
		}
		if int64(r.at)+int64(length) > int64(len(r.buf)) {
			return Value{}, errors.New("postgres: truncated reply")
		}
		out := make([]byte, length)
		copy(out, r.buf[r.at:r.at+int(length)])
		r.at += int(length)
		return Value{Type: kind, Bytes: out}, nil
	case typeTexts:
		count, err := r.u32()
		if err != nil {
			return Value{}, err
		}
		items := make([]string, 0, count)
		for index := uint32(0); index < count; index++ {
			text, err := r.str()
			if err != nil {
				return Value{}, err
			}
			items = append(items, text)
		}
		return Value{Type: kind, Texts: items}, nil
	}
	return Value{}, fmt.Errorf("postgres: unknown cell type %d", kind)
}

// header reads the part every reply carries and turns a refusal into an Error.
func header(body []byte) (*reader, error) {
	r := &reader{buf: body}
	status, err := r.u32()
	if err != nil {
		return nil, err
	}
	sqlstate, err := r.str()
	if err != nil {
		return nil, err
	}
	message, err := r.str()
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &Error{Status: status, SQLState: sqlstate, Message: message}
	}
	return r, nil
}

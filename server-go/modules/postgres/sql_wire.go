package postgres

import (
	"encoding/binary"
	"errors"
	"math"
)

// The storage wire: generic SQL for whichever module owns the tables.
//
// Nothing domain-specific belongs here. The postgres module owns PostgreSQL --
// the pool, transactions, schema versioning -- and every module that owns rows
// reaches them through this. What a row MEANS is the caller's, which is why
// this file knows about types and sizes and knows nothing about memories,
// documents or grants.
//
// The encoding is agreed with the db1 module rather than invented here, so
// there is one codec and not two. Everything is little-endian; a string is a
// u32 byte length followed by that many bytes.

// Operations. The numbers are the contract's, not an internal enum -- renaming
// a constant here does not change what a peer sends.
const (
	OpExec           uint32 = 1
	OpQuery          uint32 = 2
	OpBegin          uint32 = 3
	OpCommit         uint32 = 4
	OpRollback       uint32 = 5
	OpMigrate        uint32 = 6
	OpCurrentVersion uint32 = 7
)

// Value types, one set for arguments and result cells alike.
//
// Sharing the set is deliberate: two sets drift, and a cell type that exists
// only in one direction is a conversion nobody wrote down. Typed rather than
// text for the reason the tree keeps rediscovering -- a BOOLEAN column read as
// text is a parse, and a parse is a guess. The C read mining_jobs.enabled with
// atoi and reported every job disabled, because atoi("t") is 0.
const (
	ValueNull  uint8 = 0
	ValueText  uint8 = 1
	ValueInt   uint8 = 2
	ValueFloat uint8 = 3
	ValueBool  uint8 = 4
	ValueTexts uint8 = 5
	// Appended, never inserted: renumbering a live type set reinterprets
	// every value already encoded against it.
	ValueBytes uint8 = 6
)

// Limits, enforced here rather than trusted from the sender.
//
// The client enforces its own copy before sending, which makes for a better
// error and no round trip. That is a courtesy, not a control: a limit checked
// only by the sender holds until the sender is buggy, old, or not the one we
// agreed with.
const (
	MaxStatementBytes   = 1 << 20
	MaxArguments        = 4096
	MaxCellBytes        = 1 << 20
	MaxStatementIDBytes = 64
	MaxReplyRows        = 4096
	MaxOwnerBytes       = 64
	MaxMigrationCount   = 4096
)

// Reply statuses. A refusal says which kind it is, because a caller retries a
// serialization failure and does not retry a malformed request.
// These numbers are the contract, agreed with db1 and pinned on both sides.
// Renaming a constant here changes nothing; changing a number changes what the
// far end believes happened, and a size refusal carries no SQLSTATE to catch it
// with.
const (
	StatusOK             uint32 = 0
	StatusInvalidRequest uint32 = 1
	StatusUnsupported    uint32 = 2
	// 3 and 4 are db1's StoreStatusLimitExceeded and StoreStatusFailed.
	StatusLimitExceeded   uint32 = 3
	StatusStatementFailed uint32 = 4
	StatusUnavailable     uint32 = 5
	StatusMigrationFailed uint32 = 6

	// statusCount is how many of the above exist, so the test that pins their
	// values can also assert it is pinning ALL of them. Without it, a status
	// added here and used on the wire but never added to that list crosses
	// unchecked and every suite stays green.
	//
	// Bump this in the same edit that adds a status. That is the point: the
	// test fails until the new one is pinned too.
	statusCount = 7
)

var errMalformed = errors.New("postgres: malformed storage request")

// Value is one argument or result cell. Null is its own type rather than a
// zero, because a column that is NULL and a column that is empty are different
// facts and a wire that flattens them makes the difference unrecoverable.
type Value struct {
	Type  uint8
	Text  string
	Int   int64
	Float float64
	Bool  bool
	Texts []string
	// Nil is NULL; a non-nil zero-length slice is an empty value. []byte
	// carries that distinction natively, so the round trip is exact in both
	// directions without the pointer every other type needs.
	Bytes []byte
}

// Request is one decoded storage call.
type Request struct {
	Op          uint32
	StatementID string
	TxHandle    uint64
	SQL         string
	Args        []Value
	Owner       string
	Version     uint64
	Checksum    string
	Statements  []string
}

type reader struct {
	buf []byte
	at  int
}

func (r *reader) u32() (uint32, error) {
	if r.at+4 > len(r.buf) {
		return 0, errMalformed
	}
	value := binary.LittleEndian.Uint32(r.buf[r.at:])
	r.at += 4
	return value, nil
}

func (r *reader) u64() (uint64, error) {
	if r.at+8 > len(r.buf) {
		return 0, errMalformed
	}
	value := binary.LittleEndian.Uint64(r.buf[r.at:])
	r.at += 8
	return value, nil
}

func (r *reader) u8() (uint8, error) {
	if r.at+1 > len(r.buf) {
		return 0, errMalformed
	}
	value := r.buf[r.at]
	r.at++
	return value, nil
}

// str reads a length-prefixed string, refusing one longer than the caller
// allows. The bound is passed in rather than fixed so the same reader serves a
// 64-byte statement id and a one-megabyte statement.
func (r *reader) str(maximum int) (string, error) {
	length, err := r.u32()
	if err != nil {
		return "", err
	}
	if int(length) > maximum || r.at+int(length) > len(r.buf) {
		return "", errMalformed
	}
	value := string(r.buf[r.at : r.at+int(length)])
	r.at += int(length)
	return value, nil
}

func (r *reader) value() (Value, error) {
	kind, err := r.u8()
	if err != nil {
		return Value{}, err
	}
	switch kind {
	case ValueNull:
		return Value{Type: kind}, nil
	case ValueText:
		text, err := r.str(MaxCellBytes)
		return Value{Type: kind, Text: text}, err
	case ValueInt:
		raw, err := r.u64()
		return Value{Type: kind, Int: int64(raw)}, err
	case ValueFloat:
		raw, err := r.u64()
		return Value{Type: kind, Float: math.Float64frombits(raw)}, err
	case ValueBool:
		raw, err := r.u8()
		if err != nil {
			return Value{}, err
		}
		if raw > 1 {
			// A bool is 0 or 1. Anything else is a sender that thinks this
			// field means something it does not.
			return Value{}, errMalformed
		}
		return Value{Type: kind, Bool: raw == 1}, nil
	case ValueBytes:
		length, err := r.u32()
		if err != nil {
			return Value{}, err
		}
		if int(length) > MaxCellBytes || r.at+int(length) > len(r.buf) {
			return Value{}, errMalformed
		}
		// Copied rather than aliased: the caller's buffer is reused for the
		// next frame, and a value that changes under the handler is worse
		// than one that costs an allocation.
		raw := make([]byte, length)
		copy(raw, r.buf[r.at:r.at+int(length)])
		r.at += int(length)
		return Value{Type: kind, Bytes: raw}, nil
	case ValueTexts:
		count, err := r.u32()
		if err != nil {
			return Value{}, err
		}
		if int(count) > MaxArguments {
			return Value{}, errMalformed
		}
		items := make([]string, 0, count)
		for index := uint32(0); index < count; index++ {
			text, err := r.str(MaxCellBytes)
			if err != nil {
				return Value{}, err
			}
			items = append(items, text)
		}
		return Value{Type: kind, Texts: items}, nil
	}
	return Value{}, errMalformed
}

// DecodeRequest reads one call, refusing anything past a limit rather than
// truncating it. Every bound is checked here so a handler never sees an
// oversized field.
func DecodeRequest(body []byte) (Request, error) {
	r := &reader{buf: body}
	op, err := r.u32()
	if err != nil {
		return Request{}, err
	}
	request := Request{Op: op}

	switch op {
	case OpMigrate:
		if request.Owner, err = r.str(MaxOwnerBytes); err != nil {
			return Request{}, err
		}
		if request.Version, err = r.u64(); err != nil {
			return Request{}, err
		}
		if request.Checksum, err = r.str(128); err != nil {
			return Request{}, err
		}
		count, err := r.u32()
		if err != nil {
			return Request{}, err
		}
		if int(count) > MaxMigrationCount {
			return Request{}, errMalformed
		}
		request.Statements = make([]string, 0, count)
		for index := uint32(0); index < count; index++ {
			statement, err := r.str(MaxStatementBytes)
			if err != nil {
				return Request{}, err
			}
			request.Statements = append(request.Statements, statement)
		}
	case OpCurrentVersion:
		if request.Owner, err = r.str(MaxOwnerBytes); err != nil {
			return Request{}, err
		}
	case OpExec, OpQuery, OpBegin, OpCommit, OpRollback:
		if request.StatementID, err = r.str(MaxStatementIDBytes); err != nil {
			return Request{}, err
		}
		if request.TxHandle, err = r.u64(); err != nil {
			return Request{}, err
		}
		if request.SQL, err = r.str(MaxStatementBytes); err != nil {
			return Request{}, err
		}
		count, err := r.u32()
		if err != nil {
			return Request{}, err
		}
		if int(count) > MaxArguments {
			return Request{}, errMalformed
		}
		request.Args = make([]Value, 0, count)
		for index := uint32(0); index < count; index++ {
			value, err := r.value()
			if err != nil {
				return Request{}, err
			}
			request.Args = append(request.Args, value)
		}
	default:
		return Request{}, errMalformed
	}
	if r.at != len(body) {
		// Trailing bytes mean the sender and this disagree about the frame.
		// Ignoring them would let a version skew read as a valid call.
		return Request{}, errMalformed
	}
	return request, nil
}

type writer struct{ buf []byte }

func (w *writer) u32(value uint32) {
	var raw [4]byte
	binary.LittleEndian.PutUint32(raw[:], value)
	w.buf = append(w.buf, raw[:]...)
}

func (w *writer) u64(value uint64) {
	var raw [8]byte
	binary.LittleEndian.PutUint64(raw[:], value)
	w.buf = append(w.buf, raw[:]...)
}

func (w *writer) str(value string) {
	w.u32(uint32(len(value)))
	w.buf = append(w.buf, value...)
}

func (w *writer) value(value Value) {
	w.buf = append(w.buf, value.Type)
	switch value.Type {
	case ValueText:
		w.str(value.Text)
	case ValueInt:
		w.u64(uint64(value.Int))
	case ValueFloat:
		w.u64(math.Float64bits(value.Float))
	case ValueBool:
		if value.Bool {
			w.buf = append(w.buf, 1)
		} else {
			w.buf = append(w.buf, 0)
		}
	case ValueBytes:
		w.u32(uint32(len(value.Bytes)))
		w.buf = append(w.buf, value.Bytes...)
	case ValueTexts:
		w.u32(uint32(len(value.Texts)))
		for _, text := range value.Texts {
			w.str(text)
		}
	}
}

// header writes the part every reply carries. sqlstate and message are empty on
// success; on failure the SQLSTATE crosses so a caller can tell a unique
// violation (23505) from a foreign key violation (23503) from an outage, which
// want three different responses.
func replyHeader(status uint32, sqlstate, message string) *writer {
	w := &writer{}
	w.u32(status)
	w.str(sqlstate)
	w.str(message)
	return w
}

// EncodeError is a refusal with no payload.
func EncodeError(status uint32, sqlstate, message string) []byte {
	return replyHeader(status, sqlstate, message).buf
}

// EncodeExecReply answers how many rows the statement changed.
func EncodeExecReply(rowsAffected uint64) []byte {
	w := replyHeader(StatusOK, "", "")
	w.u64(rowsAffected)
	return w.buf
}

// EncodeQueryReply answers a result set, width first so a reader can size a row
// before reading one.
func EncodeQueryReply(width uint32, rows [][]Value) []byte {
	w := replyHeader(StatusOK, "", "")
	w.u32(width)
	w.u32(uint32(len(rows)))
	for _, row := range rows {
		for _, cell := range row {
			w.value(cell)
		}
	}
	return w.buf
}

// EncodeBeginReply answers the handle the caller puts on every statement in the
// transaction.
func EncodeBeginReply(handle uint64) []byte {
	w := replyHeader(StatusOK, "", "")
	w.u64(handle)
	return w.buf
}

// EncodeAckReply is COMMIT, ROLLBACK and MIGRATE: the status is the answer.
func EncodeAckReply() []byte {
	return replyHeader(StatusOK, "", "").buf
}

// EncodeCurrentVersionReply answers the highest recorded version for an owner
// and the checksum it was recorded with, so a caller can verify its own file
// against the head before sending anything.
func EncodeCurrentVersionReply(version uint64, checksum string) []byte {
	w := replyHeader(StatusOK, "", "")
	w.u64(version)
	w.str(checksum)
	return w.buf
}

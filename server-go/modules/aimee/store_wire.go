package aimee

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"strconv"
)

// The frame aimee and the postgres module agreed.
//
// Kept apart from the client so the encoding can be read, tested and reconciled
// on its own. The postgres module implements the other half; if the two ever
// disagree the disagreement is in this file.
//
// VALUES CARRY THEIR TYPE, both directions. The alternative -- everything as
// text, typed by whatever the far end guesses -- is a bug generator, and both
// sides have shipped one this month. A BOOLEAN column read through atoi gives
// atoi("t") == 0, so every row reads false, including the rows whose default is
// true; the same column scanned into an int64 on the Go side made an operation
// answer "no such row" for a row that was there. A type byte costs one byte and
// removes the category.
//
// NULL IS ITS OWN TYPE, not an empty string. These tables distinguish the two:
// a NULL heartbeat means "never beaten" rather than "beat at the zero time",
// and a column that used 0 for "unassigned" could not carry a foreign key until
// it became nullable. A wire that flattens them makes the distinction
// unrecoverable at the far end.

// Wire value types. One set, used for statement arguments and result cells
// alike, so neither direction can drift from the other.
const (
	wireNull uint8 = iota
	wireText
	wireInt
	wireFloat
	wireBool
	wireTextArray
	// wireBytes is a BYTEA column: raw bytes, not text that happens to be
	// valid UTF-8. aimee has real ones -- the JWKS digests are 32 raw bytes each
	// and the nonce table's primary key is 32 more -- and carrying them as text
	// would mean choosing an encoding on one side and guessing it on the other.
	//
	// Appended rather than inserted: the numbering above is agreed with the
	// store, and renumbering a live type set silently reinterprets every value.
	wireBytes
)

// Operations aimee asks of the store.
//
// iota, and its absence here was a real defect for as long as this file has
// existed: without it a const block repeats the EXPRESSION, so `= 1` followed by
// six bare names made all seven constants 1. Every operation -- query, begin,
// commit, rollback, migrate, current_version -- went onto the wire as EXEC.
//
// Nothing caught it because nothing served the other end. The store module's
// SQL stage did not exist, so every call failed at the transport before its
// opcode was ever read, and the first thing to notice was the Go compiler
// refusing a switch with seven identical cases when that stage was written.
// A wire constant is only checked by the far side reading it.
const (
	opStoreExec uint32 = iota + 1
	opStoreQuery
	opStoreBegin
	opStoreCommit
	opStoreRollback
	// MIGRATE applies an owner's versioned schema change and records it. DDL
	// never travels through EXEC: a client holding EXEC could otherwise reshape
	// any table in the database, and every reshape would be unrecorded.
	opStoreMigrate
	// CURRENT_VERSION asks how far an owner's schema history has been applied.
	opStoreCurrentVersion
)

// Limits, stated rather than discovered. aimee's widest request is 387 fields and
// its largest repeated block is 512 models at six fields each, so 4096 leaves
// room without re-capping something already capped upstream.
//
// Checked here as a fast local failure -- a better message and no round trip --
// but the STORE enforces them. A limit the sender checks is a convention, and
// the module has to hold against a client that is buggy, old, or not this one.
const (
	MaxStatementBytes = 1 << 20
	MaxArgs           = 4096
	MaxRowsPerReply   = 4096
	MaxCellBytes      = 1 << 20
	MaxStatementID    = 64
)

// Statuses the STORE answers with. These belong to the store contract and are
// pinned here as numbers, because a status is only useful if both sides agree
// what the integer means.
//
// They were previously borrowed from aimee's own five operation result codes,
// which happened to produce the right bytes and was still wrong: the store wire
// and aimee's operation wire are different contracts between different parties,
// and a shared constant made the store's values look decided when nobody had
// written them down. The failure that would have caused is quiet rather than
// loud -- if the two sides disagreed about which integer means "refused for
// size", an over-ceiling refusal would carry no SQLSTATE, fall through to the
// classifier, and be recorded as an unexplained failure instead of a result too
// large to send. Everything else about the call would look normal.
//
// The values match what the wire already carried, so pinning them changes no
// bytes. What it changes is that they are now stated.
const (
	StoreStatusOK            uint32 = 0
	StoreStatusLimitExceeded uint32 = 3
	StoreStatusFailed        uint32 = 4
)

// THEY HAPPEN TO AGREE WITH THE OPERATION STATUSES IN wire.go. DO NOT RELY ON IT.
//
// StatusOK is also 0, StatusTooLong is also 3, StatusFailed is also 4, and the
// meanings line up closely enough that passing one through as the other would
// look right and mostly BE right. That is the coincidence worth naming, because
// it is the kind that stops being true without anything looking wrong.
//
// The two are separate contracts with separate owners. These are agreed with the
// postgres module and can grow a value this module does not have -- a status for
// "the store is at its transaction cap" was discussed today and would land here
// and nowhere else. The operation statuses are the catalog's five, shared with
// 461 C call sites, and cannot grow a sixth without their agreement.
//
// So nothing converts between them. A store reply becomes an ERROR
// (ErrResultTooLarge, ErrTxClosed, StoreError) and a family decides what
// operation status that error deserves. The families reference no StoreStatus at
// all, which is the property to preserve: the day the two enums diverge, code
// that assigned one to the other keeps compiling and starts lying.
//
// The peer-messaging module hit the sharp version of this: its status 1 is
// no_peer where this module's 1 is MISSING, and its 4 is hop_limit where this
// module's 4 is FAILED. Same integers, unrelated meanings, and a value that
// reads as sensible either way.

// PostgreSQL's own SQLSTATEs for a transaction that is no longer usable. Using
// the real codes rather than inventing a status keeps the reply frame as it is:
// the store already carries a SQLSTATE, and these are what PostgreSQL itself
// answers when a statement arrives outside a live transaction.
const (
	sqlStateNoActiveTransaction = "25P01"
	sqlStateInFailedTransaction = "25P02"
)

// ErrResultTooLarge is a result set the wire cannot carry.
//
// Never a truncation. A caller handed the ceiling could not tell a complete
// answer from a capped one, and would record the cap as fact -- so the store
// refuses instead, and a caller that needs more must page. aimee sends an explicit
// LIMIT on every list, so reaching this means a caller asked for more than the
// wire carries: a defect rather than a result.
var ErrResultTooLarge = errors.New("aimee: the result set exceeds what the wire carries")

// StoreError is a refusal the store explained. SQLState is what makes it
// actionable: a unique violation is a replay, a foreign-key violation is a
// caller error, and a lost connection is an outage. Collapsing those into one
// "failed" is what aimee did before this crossed.
type StoreError struct {
	SQLState string
	Message  string
	Op       string
}

func (e *StoreError) Error() string {
	if e.SQLState == "" {
		return fmt.Sprintf("aimee: the store refused %s: %s", e.Op, e.Message)
	}
	return fmt.Sprintf("aimee: the store refused %s (SQLSTATE %s): %s",
		e.Op, e.SQLState, e.Message)
}

// The SQLSTATEs aimee acts on. Others are reported but not classified.
const (
	sqlStateUniqueViolation     = "23505"
	sqlStateForeignKeyViolation = "23503"
	sqlStateCheckViolation      = "23514"
	sqlStateNotNullViolation    = "23502"
)

func sqlStateIs(err error, state string) bool {
	var se *StoreError
	return errors.As(err, &se) && se.SQLState == state
}

// IsUniqueViolation reports a row that is already there. Usually an expected
// answer -- a replayed request, a second claim on a taken slot -- rather than a
// failure.
func IsUniqueViolation(err error) bool { return sqlStateIs(err, sqlStateUniqueViolation) }

// IsForeignKeyViolation reports a reference to something absent: the caller
// named a parent that does not exist, or removed one that is still referenced.
func IsForeignKeyViolation(err error) bool { return sqlStateIs(err, sqlStateForeignKeyViolation) }

// IsCheckViolation reports a value the schema refuses.
func IsCheckViolation(err error) bool { return sqlStateIs(err, sqlStateCheckViolation) }

// IsNotNullViolation reports a required column left empty.
func IsNotNullViolation(err error) bool { return sqlStateIs(err, sqlStateNotNullViolation) }

// StoreChecksum is sha256 over a migration's statements, as applied, in order.
//
// Each statement is written as `len(statement) NUL statement NUL`. Length
// prefixing rather than a separator alone is what keeps a split from colliding
// with a join: ["A", "B"] and ["AB"] must not hash the same, and with only a
// separator they could.
//
// Both sides compute this. The store recomputes over what it actually received
// instead of recording the checksum it was handed -- trusting that would defeat
// the one thing a checksum is for, a migration whose statements changed while
// its checksum did not -- so the constructions must agree byte for byte. The
// vectors below pin them.
func StoreChecksum(statements []string) string {
	h := sha256.New()
	for _, s := range statements {
		h.Write([]byte(strconv.Itoa(len(s))))
		h.Write([]byte{0})
		h.Write([]byte(s))
		h.Write([]byte{0})
	}
	return hex.EncodeToString(h.Sum(nil))
}

// Known answers for StoreChecksum, so either implementation can be checked
// against the contract without the other one running.
const (
	// ["CREATE TABLE t (a int);"]
	checksumVectorOne = "494759c5f8401e611c3f18d05102716546f00a8272aae8202177f27aac70dcae"
	// ["A", "B"]
	checksumVectorTwo = "bfae6e09d952d65a7d2bd060a949612d0c4e2c0168dca56bc7485d5058c0d600"
	// ["AB"] -- must differ from the two above, which is the point of the
	// length prefix.
	checksumVectorJoined = "e68e79de268f9d2a92eb58a97f8deb11cb0040701bc469d9879ca16de8d7f898"
	// no statements at all
	checksumVectorEmpty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
)

// --- writing -----------------------------------------------------------------

type frameWriter struct{ buf []byte }

func (w *frameWriter) u32(v uint32) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	w.buf = append(w.buf, b[:]...)
}

func (w *frameWriter) u64(v uint64) {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], v)
	w.buf = append(w.buf, b[:]...)
}

func (w *frameWriter) str(s string) {
	w.u32(uint32(len(s)))
	w.buf = append(w.buf, s...)
}

// value writes one typed value, for an argument or a cell.
func (w *frameWriter) value(v any) error {
	switch t := v.(type) {
	case nil:
		w.buf = append(w.buf, wireNull)
	case string:
		if len(t) > MaxCellBytes {
			return fmt.Errorf("aimee: value of %d bytes exceeds the %d-byte limit",
				len(t), MaxCellBytes)
		}
		w.buf = append(w.buf, wireText)
		w.str(t)
	case int:
		w.buf = append(w.buf, wireInt)
		w.u64(uint64(int64(t)))
	case int64:
		w.buf = append(w.buf, wireInt)
		w.u64(uint64(t))
	case float64:
		w.buf = append(w.buf, wireFloat)
		w.u64(math.Float64bits(t))
	case bool:
		w.buf = append(w.buf, wireBool)
		if t {
			w.buf = append(w.buf, 1)
		} else {
			w.buf = append(w.buf, 0)
		}
	case []byte:
		// A nil slice is NULL, a non-nil empty slice is an empty bytea. Unlike
		// the string case there is nothing lost here: []byte distinguishes the
		// two natively, so the round trip is exact in both directions.
		if t == nil {
			w.buf = append(w.buf, wireNull)
			break
		}
		if len(t) > MaxCellBytes {
			return fmt.Errorf("aimee: value of %d bytes exceeds the %d-byte limit",
				len(t), MaxCellBytes)
		}
		w.buf = append(w.buf, wireBytes)
		w.u32(uint32(len(t)))
		w.buf = append(w.buf, t...)
	case []string:
		if len(t) > MaxArgs {
			return fmt.Errorf("aimee: array of %d values exceeds the %d limit",
				len(t), MaxArgs)
		}
		w.buf = append(w.buf, wireTextArray)
		w.u32(uint32(len(t)))
		for _, s := range t {
			w.str(s)
		}
	default:
		return fmt.Errorf("aimee: cannot send %T to the store", v)
	}
	return nil
}

// encodeStatement frames one statement: which operation, who is asking, which
// transaction, the SQL, and its arguments.
//
// statementID is aimee's OPERATION name. The store does not parse SQL, so this is
// the only thing that lets it log, rate-limit or authorize per operation rather
// than per connection -- and taking it from the operation table means it cannot
// drift from something real or rot at a copy-paste.
func encodeStatement(op uint32, statementID string, handle uint64,
	sql string, args []any) ([]byte, error) {
	if len(statementID) > MaxStatementID {
		return nil, fmt.Errorf("aimee: statement id %q exceeds %d bytes",
			statementID, MaxStatementID)
	}
	if len(sql) > MaxStatementBytes {
		return nil, fmt.Errorf("aimee: statement of %d bytes exceeds the %d-byte limit",
			len(sql), MaxStatementBytes)
	}
	if len(args) > MaxArgs {
		return nil, fmt.Errorf("aimee: %d arguments exceeds the %d limit",
			len(args), MaxArgs)
	}
	w := &frameWriter{buf: make([]byte, 0, 64+len(sql))}
	w.u32(op)
	w.str(statementID)
	w.u64(handle)
	w.str(sql)
	w.u32(uint32(len(args)))
	for _, a := range args {
		if err := w.value(a); err != nil {
			return nil, err
		}
	}
	return w.buf, nil
}

// encodeMigrate frames one versioned schema change: whose it is, which version,
// what it hashes to, and the statements in order.
func encodeMigrate(m MigrationRequest) ([]byte, error) {
	w := &frameWriter{}
	w.u32(opStoreMigrate)
	w.str(m.Owner)
	w.u64(uint64(m.Version))
	w.str(m.Checksum)
	w.u32(uint32(len(m.Statements)))
	for _, s := range m.Statements {
		if len(s) > MaxStatementBytes {
			return nil, fmt.Errorf("aimee: migration statement of %d bytes exceeds "+
				"the %d-byte limit", len(s), MaxStatementBytes)
		}
		w.str(s)
	}
	return w.buf, nil
}

func encodeCurrentVersion(owner string) []byte {
	w := &frameWriter{}
	w.u32(opStoreCurrentVersion)
	w.str(owner)
	return w.buf
}

// MigrationRequest is one versioned schema change, as it crosses.
type MigrationRequest struct {
	Owner      string
	Version    int64
	Checksum   string
	Statements []string
}

// --- reading -----------------------------------------------------------------

var errShortFrame = errors.New("aimee: the store's frame ended early")

type frameReader struct {
	buf []byte
	at  int
}

func (r *frameReader) u32() (uint32, error) {
	if r.at+4 > len(r.buf) {
		return 0, errShortFrame
	}
	v := binary.LittleEndian.Uint32(r.buf[r.at:])
	r.at += 4
	return v, nil
}

func (r *frameReader) u64() (uint64, error) {
	if r.at+8 > len(r.buf) {
		return 0, errShortFrame
	}
	v := binary.LittleEndian.Uint64(r.buf[r.at:])
	r.at += 8
	return v, nil
}

func (r *frameReader) byte1() (uint8, error) {
	if r.at+1 > len(r.buf) {
		return 0, errShortFrame
	}
	v := r.buf[r.at]
	r.at++
	return v, nil
}

func (r *frameReader) str() (string, error) {
	n, err := r.u32()
	if err != nil {
		return "", err
	}
	if int(n) > MaxCellBytes || r.at+int(n) > len(r.buf) {
		return "", errShortFrame
	}
	s := string(r.buf[r.at : r.at+int(n)])
	r.at += int(n)
	return s, nil
}

// blob reads a length-prefixed run of raw bytes. It copies rather than aliasing
// r.buf, because the frame is reused and a caller keeping the slice would watch
// its value change under it.
func (r *frameReader) blob() ([]byte, error) {
	n, err := r.u32()
	if err != nil {
		return nil, err
	}
	if int(n) > MaxCellBytes || r.at+int(n) > len(r.buf) {
		return nil, errShortFrame
	}
	b := make([]byte, n)
	copy(b, r.buf[r.at:r.at+int(n)])
	r.at += int(n)
	return b, nil
}

// cell is one typed value read off the wire. Kind is what the store said it
// was, which is what lets a scan target be filled exactly rather than parsed.
type cell struct {
	kind uint8
	text string
	num  int64
	real float64
	flag bool
	blob []byte
}

func (r *frameReader) value() (cell, error) {
	kind, err := r.byte1()
	if err != nil {
		return cell{}, err
	}
	c := cell{kind: kind}
	switch kind {
	case wireNull:
		return c, nil
	case wireText:
		c.text, err = r.str()
	case wireInt:
		var u uint64
		u, err = r.u64()
		c.num = int64(u)
	case wireFloat:
		var u uint64
		u, err = r.u64()
		c.real = math.Float64frombits(u)
	case wireBool:
		var b uint8
		b, err = r.byte1()
		c.flag = b != 0
	case wireBytes:
		c.blob, err = r.blob()
	case wireTextArray:
		// Not something a column returns, but the type set is shared, so
		// refusing it here says so rather than mis-reading the rest of the row.
		return cell{}, errors.New("aimee: the store sent an array as a result cell")
	default:
		return cell{}, fmt.Errorf("aimee: the store sent an unknown value type %d", kind)
	}
	return c, err
}

// storeReply is the store's answer: a status, its SQLSTATE when it refused, and
// whatever the operation returns.
type storeReply struct {
	r frameReader
}

// decodeReply reads the header every reply carries and reports a refusal as a
// StoreError, so the caller sees WHY rather than only that it failed.
func decodeReply(body []byte, op string) (*storeReply, error) {
	rep := &storeReply{r: frameReader{buf: body}}
	status, err := rep.r.u32()
	if err != nil {
		return nil, err
	}
	sqlstate, err := rep.r.str()
	if err != nil {
		return nil, err
	}
	message, err := rep.r.str()
	if err != nil {
		return nil, err
	}
	if status == StoreStatusLimitExceeded {
		// The store refused rather than truncating. Reported as its own error so
		// a caller cannot mistake it for an empty or a complete answer.
		return nil, fmt.Errorf("%w: %s", ErrResultTooLarge, message)
	}
	if status != StoreStatusOK {
		e := &StoreError{SQLState: sqlstate, Message: message, Op: op}
		if sqlstate == sqlStateNoActiveTransaction ||
			sqlstate == sqlStateInFailedTransaction {
			// The handle is gone -- expired while idle, or reclaimed when a
			// connection dropped. This has to be distinguishable from a
			// statement the store refused on its merits: the first means every
			// write in this transaction is already lost and the caller must
			// start over, the second means one statement was wrong and the
			// transaction is still live. Reported as ErrTxClosed so a caller
			// that already handles a finished transaction handles this too.
			return nil, fmt.Errorf("%w: %w", ErrTxClosed, e)
		}
		return nil, e
	}
	return rep, nil
}

// rows reads a result set: width, row count, then width*count typed cells.
func (rep *storeReply) rows() ([]cell, int, error) {
	width, err := rep.r.u32()
	if err != nil {
		return nil, 0, err
	}
	count, err := rep.r.u32()
	if err != nil {
		return nil, 0, err
	}
	if count > MaxRowsPerReply {
		// A backstop, not the enforcement: the store refuses over-large results
		// with StatusTooLong before they are framed. Reaching here means the two
		// sides disagree about the ceiling, which is worth saying plainly.
		return nil, 0, fmt.Errorf("%w: the store framed %d rows, over the %d limit",
			ErrResultTooLarge, count, MaxRowsPerReply)
	}
	if width == 0 {
		return nil, 0, nil
	}
	cells := make([]cell, 0, int(width)*int(count))
	for i := 0; i < int(width)*int(count); i++ {
		c, err := rep.r.value()
		if err != nil {
			return nil, 0, err
		}
		cells = append(cells, c)
	}
	return cells, int(width), nil
}

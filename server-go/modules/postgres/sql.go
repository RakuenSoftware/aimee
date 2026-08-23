package postgres

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"
)

// The storage stage: SQL on behalf of whichever module owns the tables.
//
// This module owns PostgreSQL and nothing else -- no memories, no documents, no
// grants. A caller brings its own statement and its own meaning; what this
// contributes is the pool, transactions with an owner, schema versioning, and
// bounds that hold whether or not the caller checks its own.

const (
	// EventSQL is postgres stage 2. The kind is derived the way every other
	// component's is: 4097 + 256*principal_ref + (stage - 1), with postgres at
	// ref 28.
	EventSQL uint32 = 11266
	StageSQL uint32 = 2
)

// SQLHandler serves the storage stage.
type SQLHandler struct {
	store        *poolSchemaStore
	transactions *transactionTable
}

// NewSQLHandler builds the stage over an already-open pool.
func NewSQLHandler() *SQLHandler {
	return &SQLHandler{store: &poolSchemaStore{}, transactions: newTransactionTable()}
}

// classifyFailure decides which fact a driver error actually carries, and which
// five-character SQLSTATE says so.
//
// The SQLSTATE crosses the wire because a caller cannot retry sensibly without
// it: a unique violation is often an expected answer, a foreign key violation is
// a caller error, and a lost connection is an outage. Collapsing all three into
// "failed" is what forces a caller to guess, and db1 today has a comment working
// around exactly that absence.
//
// If PostgreSQL rejected the statement it sent an ErrorResponse, which pgx
// surfaces as a *pgconn.PgError with a SQLSTATE. So the absence of a PgError is
// evidence, not an absence of evidence: it means the server never rejected
// anything, and reporting "statement failed" would send the caller to rewrite a
// statement that was never read.
//
// Two other outcomes can be named. The caller's deadline expired, which is a
// cancelled statement (57014, the same code PostgreSQL returns when its own
// statement_timeout fires). Or the connection failed, which is an outage
// (08006) and a different repair entirely.
//
// Anything else keeps StatusStatementFailed. Guessing "unavailable" for an
// unfamiliar client-side error would be this same collapse pointing the other
// way.
func classifyFailure(err error) (uint32, string, string) {
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		return StatusStatementFailed, pgErr.Code, err.Error()
	}
	if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) {
		return StatusStatementFailed, sqlStateQueryCanceled, err.Error()
	}
	var connErr *pgconn.ConnectError
	var netErr net.Error
	if errors.As(err, &connErr) || errors.As(err, &netErr) ||
		errors.Is(err, net.ErrClosed) || errors.Is(err, io.EOF) ||
		errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, syscall.ECONNRESET) ||
		errors.Is(err, syscall.EPIPE) {
		return StatusUnavailable, sqlStateConnectionFailure, err.Error()
	}
	return StatusStatementFailed, "", err.Error()
}

// bind converts one wire value into something pgx can send.
//
// Null is nil rather than a zero of the column's type, because a column set to
// NULL and a column set to empty are different facts and the wire went to the
// trouble of keeping them apart.
func bind(value Value) (any, error) {
	switch value.Type {
	case ValueNull:
		return nil, nil
	case ValueText:
		return value.Text, nil
	case ValueInt:
		return value.Int, nil
	case ValueFloat:
		return value.Float, nil
	case ValueBool:
		return value.Bool, nil
	case ValueBytes:
		return value.Bytes, nil
	case ValueTexts:
		return value.Texts, nil
	}
	return nil, errMalformed
}

func bindAll(args []Value) ([]any, error) {
	out := make([]any, 0, len(args))
	for _, value := range args {
		bound, err := bind(value)
		if err != nil {
			return nil, err
		}
		out = append(out, bound)
	}
	return out, nil
}

// cell converts one scanned column into a wire value, or refuses it.
//
// The type comes from what the driver produced, not from a guess at what the
// column probably holds. That is the whole reason cells are typed: reading a
// BOOLEAN as text and parsing it back is how the C came to report every mining
// job disabled, because atoi("t") is 0.
//
// An unrecognised driver type is REFUSED rather than rendered. Rendering was the
// old answer and it was chosen against dropping the column, which is a real
// harm -- a silently narrower result set. But a rendering lands on text, and
// text is what a real column produces, so a rendered value and a genuine TEXT
// column are the same answer to a caller scanning into a string. A default is
// safe when it lands on what the other side would have written and unsafe the
// moment it does not.
//
// Refusing is neither of the two options that comparison weighed: nothing is
// dropped and nothing is guessed at, and the reply names the type that could not
// cross, which an operation author can act on.
func cell(raw any) (Value, error) {
	switch typed := raw.(type) {
	case nil:
		return Value{Type: ValueNull}, nil
	case string:
		return Value{Type: ValueText, Text: typed}, nil
	case []byte:
		return Value{Type: ValueBytes, Bytes: typed}, nil
	case bool:
		return Value{Type: ValueBool, Bool: typed}, nil
	case int16:
		return Value{Type: ValueInt, Int: int64(typed)}, nil
	case int32:
		return Value{Type: ValueInt, Int: int64(typed)}, nil
	case int64:
		return Value{Type: ValueInt, Int: typed}, nil
	case float32:
		return Value{Type: ValueFloat, Float: float64(typed)}, nil
	case float64:
		return Value{Type: ValueFloat, Float: typed}, nil
	case pgtype.Numeric:
		// SUM over a BIGINT is NUMERIC, so this is an ordinary result column and
		// not an exotic one. Rendered rather than converted, it crossed as a
		// struct dump and scanned into an integer as zero.
		//
		// Exact integers cross as integers, which is what a SUM of counts is and
		// what its callers scan it into. Anything else that fits a float crosses
		// as a float. A NUMERIC that is neither -- NaN, infinity, or wider than
		// float64 -- keeps the rendered form, where the destination check now
		// refuses it instead of writing a zero that reads as data.
		if typed.Valid && !typed.NaN && typed.InfinityModifier == pgtype.Finite {
			if whole, err := typed.Int64Value(); err == nil && whole.Valid {
				return Value{Type: ValueInt, Int: whole.Int64}, nil
			}
			if real, err := typed.Float64Value(); err == nil && real.Valid {
				return Value{Type: ValueFloat, Float: real.Float64}, nil
			}
		}
	case time.Time:
		// Stamps cross as the tree's canonical spelling rather than as a
		// driver-formatted string, so a caller reading one back gets what it
		// would have read from a TEXT column.
		return Value{Type: ValueText, Text: typed.UTC().Format("2006-01-02T15:04:05Z")}, nil
	}
	return Value{}, fmt.Errorf("no wire type for a %T column; it would cross as "+
		"text and read as one", raw)
}

// runner is whichever of the pool or an open transaction a statement runs on.
type runner interface {
	Exec(context.Context, string, ...any) (pgconn.CommandTag, error)
	Query(context.Context, string, ...any) (pgx.Rows, error)
}

// Handle serves one storage call.
func (h *SQLHandler) Handle(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	// Reclaim before serving, so a caller whose transaction has expired is told
	// so by this call rather than by a later commit that loses its writes.
	h.transactions.reapIdle(context.Background(), time.Now())

	request, err := DecodeRequest(body)
	if err != nil {
		return EncodeError(StatusInvalidRequest, "", "malformed storage request"), bus.ModuleStatusOK
	}
	if invocation.PrincipalRef == 0 {
		// The runtime could not attribute the call. A module that authorizes
		// cannot treat that as a wildcard.
		return EncodeError(StatusInvalidRequest, "", "unattributed caller"), bus.ModuleStatusOK
	}

	// The caller's deadline becomes the statement's. A query that outlives the
	// call that asked for it is holding a pooled connection for an answer
	// nobody is waiting for.
	ctx := context.Background()
	if remaining := invocation.Remaining(30 * time.Second); remaining > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, remaining)
		defer cancel()
	}

	switch request.Op {
	case OpExec, OpQuery:
		return h.statement(ctx, invocation, request)
	case OpBegin:
		return h.begin(ctx, invocation)
	case OpCommit, OpRollback:
		return h.finish(ctx, invocation, request)
	case OpMigrate:
		return h.migrate(ctx, request)
	case OpCurrentVersion:
		return h.currentVersion(ctx, request)
	}
	return EncodeError(StatusUnsupported, "", "unknown operation"), bus.ModuleStatusOK
}

// statement runs EXEC or QUERY, on the pool or inside a claimed transaction.
func (h *SQLHandler) statement(ctx context.Context, invocation bus.ModuleInvocation,
	request Request) ([]byte, bus.ModuleStatus) {
	if request.StatementID == "" {
		// Required, not optional. It is the only thing that lets this module
		// say what a caller was doing without parsing SQL, and an optional
		// field is one that rots.
		return EncodeError(StatusInvalidRequest, "", "statement_id is required"), bus.ModuleStatusOK
	}

	var run runner
	if request.TxHandle != 0 {
		entry, ok := h.transactions.claim(request.TxHandle, invocation.PrincipalRef,
			invocation.SrcHandle)
		if !ok {
			// Expired, reclaimed, or someone else's. All three mean the same
			// thing to the caller: the transaction it thinks it has is gone.
			return EncodeError(StatusStatementFailed, sqlStateNoActiveTransaction,
				"transaction is not open"), bus.ModuleStatusOK
		}
		run = entry.tx
	} else {
		pool, err := productionProbe.getPool()
		if err != nil {
			return EncodeError(StatusUnavailable, "", "database unavailable"), bus.ModuleStatusOK
		}
		run = pool
	}

	args, err := bindAll(request.Args)
	if err != nil {
		return EncodeError(StatusInvalidRequest, "", "malformed argument"), bus.ModuleStatusOK
	}

	if request.Op == OpExec {
		tag, execErr := run.Exec(ctx, request.SQL, args...)
		if execErr != nil {
			status, state, detail := classifyFailure(execErr)
			return EncodeError(status, state, detail), bus.ModuleStatusOK
		}
		return EncodeExecReply(uint64(tag.RowsAffected())), bus.ModuleStatusOK
	}

	rows, queryErr := run.Query(ctx, request.SQL, args...)
	if queryErr != nil {
		status, state, detail := classifyFailure(queryErr)
		return EncodeError(status, state, detail), bus.ModuleStatusOK
	}
	defer rows.Close()

	return collectReply(rows), bus.ModuleStatusOK
}

// collectReply turns a result set into a reply, or into the refusal its size
// earns.
//
// A function of its own because both ceilings below are refusals that nothing
// could reach where they used to live: inside the statement path, exercising
// them meant a database with four thousand columns or a stub of every pgx.Tx
// method. Taking a pgx.Rows means a dozen-line fake can drive both, which is
// the difference between a bound that is checked and one that is asserted.
func collectReply(rows pgx.Rows) []byte {
	columns := len(rows.FieldDescriptions())
	if columns > MaxReplyColumns {
		// Refused rather than narrowed. The width crosses as a four-byte field,
		// and a width that wrapped would tell the reader to size every row wrong
		// -- which is not a bad number, it is a misparse of everything after it.
		return EncodeError(StatusLimitExceeded, sqlStateResultTooLarge,
			fmt.Sprintf("result has %d columns, over %d", columns, MaxReplyColumns))
	}
	width := uint32(columns)
	collected := make([][]Value, 0, 64)
	// Status, empty sqlstate, empty message, width, count: what a reply carries
	// before any row does.
	size := 4 + 4 + 4 + 4 + 4
	for rows.Next() {
		raw, valueErr := rows.Values()
		if valueErr != nil {
			status, state, detail := classifyFailure(valueErr)
			return EncodeError(status, state, detail)
		}
		if len(collected) >= MaxReplyRows {
			// Refused, not truncated. A caller handed exactly the ceiling
			// cannot tell a complete answer from a capped one, and will record
			// the cap as fact. Paging is the caller's to do; guessing on its
			// behalf is not.
			return EncodeError(StatusLimitExceeded, sqlStateResultTooLarge,
				fmt.Sprintf("result exceeds %d rows; narrow or page the query",
					MaxReplyRows))
		}
		row := make([]Value, 0, width)
		for _, value := range raw {
			converted, cellErr := cell(value)
			if cellErr != nil {
				// Unsupported rather than statement-failed: the statement ran
				// and PostgreSQL answered. What cannot happen is carrying the
				// answer, which is this module's limitation and not the
				// caller's mistake.
				return EncodeError(StatusUnsupported, "", cellErr.Error())
			}
			size += encodedSize(converted)
			row = append(row, converted)
		}
		if size > MaxReplyBytes {
			// Checked here rather than after encoding, because the product of
			// the other bounds is gigabytes: a reply built and then rejected is
			// a reply this module allocated on a caller's say-so.
			return EncodeError(StatusLimitExceeded, sqlStateResultTooLarge,
				fmt.Sprintf("result exceeds %d bytes; narrow or page the query",
					MaxReplyBytes))
		}
		collected = append(collected, row)
	}
	if rows.Err() != nil {
		status, state, detail := classifyFailure(rows.Err())
		return EncodeError(status, state, detail)
	}
	return EncodeQueryReply(width, collected)
}

// begin opens a transaction and hands back a handle bound to the caller.
func (h *SQLHandler) begin(ctx context.Context, invocation bus.ModuleInvocation) (
	[]byte, bus.ModuleStatus,
) {
	if h.transactions.countFor(invocation.PrincipalRef) >= maxOpenPerPrincipal {
		// A transaction pins a pooled connection. One caller opening them
		// without closing them would starve every other module.
		return EncodeError(StatusLimitExceeded, sqlStateTooManyTransactions,
			fmt.Sprintf("at the limit of %d open transactions; a handle that is "+
				"never committed or rolled back costs this capacity permanently",
				maxOpenPerPrincipal)), bus.ModuleStatusOK
	}
	pool, err := productionProbe.getPool()
	if err != nil {
		return EncodeError(StatusUnavailable, "", "database unavailable"), bus.ModuleStatusOK
	}
	conn, acquireErr := pool.Acquire(ctx)
	if acquireErr != nil {
		return EncodeError(StatusUnavailable, "", "no connection available"), bus.ModuleStatusOK
	}
	tx, beginErr := conn.Begin(ctx)
	if beginErr != nil {
		conn.Release()
		status, state, detail := classifyFailure(beginErr)
		return EncodeError(status, state, detail), bus.ModuleStatusOK
	}
	handle, handleErr := newHandle()
	if handleErr != nil {
		_ = tx.Rollback(ctx)
		conn.Release()
		return EncodeError(StatusUnavailable, "", "could not mint a handle"), bus.ModuleStatusOK
	}
	h.transactions.put(handle, &openTransaction{tx: tx, conn: conn,
		principal: invocation.PrincipalRef, handle: invocation.SrcHandle,
		lastUsed: time.Now()})
	return EncodeBeginReply(handle), bus.ModuleStatusOK
}

// finish commits or rolls back, and always returns the connection.
//
// A COMMIT on a handle this no longer holds answers 25P01 rather than OK. That
// single choice is what turns "every write in the transaction was silently
// lost" into an error the caller acts on.
func (h *SQLHandler) finish(ctx context.Context, invocation bus.ModuleInvocation,
	request Request) ([]byte, bus.ModuleStatus) {
	entry, ok := h.transactions.claim(request.TxHandle, invocation.PrincipalRef,
		invocation.SrcHandle)
	if !ok {
		return EncodeError(StatusStatementFailed, sqlStateNoActiveTransaction,
			"transaction is not open"), bus.ModuleStatusOK
	}
	h.transactions.remove(request.TxHandle)
	defer entry.conn.Release()

	var err error
	if request.Op == OpCommit {
		err = entry.tx.Commit(ctx)
	} else {
		err = entry.tx.Rollback(ctx)
	}
	if err != nil {
		status, state, detail := classifyFailure(err)
		return EncodeError(status, state, detail), bus.ModuleStatusOK
	}
	return EncodeAckReply(), bus.ModuleStatusOK
}

// migrate applies one version for one owner.
func (h *SQLHandler) migrate(ctx context.Context, request Request) ([]byte, bus.ModuleStatus) {
	migration := Migration{Owner: request.Owner, Version: int64(request.Version),
		Statements: request.Statements}

	// The checksum is recomputed over the statements that actually arrived, not
	// taken from the caller. Recording the caller's would defeat the one thing
	// the checksum exists to catch: statements that changed while the checksum
	// stayed the same.
	if computed := migration.Checksum(); computed != request.Checksum {
		return EncodeError(StatusInvalidRequest, "",
			"checksum does not match the statements sent"), bus.ModuleStatusOK
	}
	if _, err := Migrate(ctx, h.store, migration); err != nil {
		// A migration PostgreSQL rejected and a migration it never received are
		// opposite repairs: the first must never be retried, the second must be.
		// Reporting both as MigrationFailed stops a rollout that a connection
		// blip would only have delayed.
		status, state, detail := classifyFailure(err)
		if status == StatusStatementFailed {
			status = StatusMigrationFailed
		}
		return EncodeError(status, state, detail), bus.ModuleStatusOK
	}
	return EncodeAckReply(), bus.ModuleStatusOK
}

// currentVersion answers an owner's recorded head.
func (h *SQLHandler) currentVersion(ctx context.Context, request Request) (
	[]byte, bus.ModuleStatus,
) {
	version, checksum, err := CurrentVersionAndChecksum(ctx, h.store, request.Owner)
	if err != nil {
		status, state, detail := classifyFailure(err)
		if status == StatusStatementFailed {
			status = StatusMigrationFailed
		}
		return EncodeError(status, state, detail), bus.ModuleStatusOK
	}
	return EncodeCurrentVersionReply(uint64(version), checksum), bus.ModuleStatusOK
}

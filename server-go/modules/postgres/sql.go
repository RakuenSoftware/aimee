package postgres

// The SQL stage: kind 11266, the half of the store contract this module owes.
//
// aimee keeps no database. It serves nineteen families of typed operations and
// reaches PostgreSQL by calling here, so until this file existed the store
// module attached, built its client, found nothing serving 11266, and exited --
// which presented as "the store is absent" on a system where every part had
// been written.
//
// The frame is the one described in server-go/db/store_wire.go, and
// that file is the specification. Nothing here re-derives it: the encodings are
// mirrored deliberately rather than shared, because the two sides are separate
// modules that must be able to version independently, and a shared struct would
// make a wire change look like a refactor. Where the two disagree, store_wire.go
// is right and this is wrong.
//
// WHAT THIS STAGE MAY NOT DO: parse or rewrite SQL. It is a transport with a
// connection pool, a transaction registry and limits. The one exception is
// MIGRATE, which is a separate operation precisely so that DDL cannot travel
// through EXEC -- a client holding EXEC would otherwise be able to reshape any
// table in the database, and every reshape would go unrecorded.

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	// EventSQL is the kind this stage listens on: 4096 + 28*256 + 2.
	EventSQL uint32 = 11266
	// StageSQL is its stage id.
	StageSQL uint32 = 2
)

// Operations, matching store_wire.go's opStore* block. Numbered, not named, on
// the wire -- so the order here is the contract.
// iota, and the client's copy of this block was missing it -- which made all
// seven of its constants 1 and every operation it sent an EXEC. The compiler
// caught it here, in a switch with seven identical cases, only because this
// stage finally reads them.
const (
	opExec uint32 = iota + 1
	opQuery
	opBegin
	opCommit
	opRollback
	opMigrate
	opCurrentVersion
)

// Value types, matching store_wire.go's wire* block.
const (
	valNull uint8 = iota
	valText
	valInt
	valFloat
	valBool
	valTextArray
	valBytes
)

// Statuses, matching store_wire.go's StoreStatus* block.
//
// THREE VALUES, NOT FIVE, and the gap at 1 and 2 is deliberate: those belong to
// aimee's own operation results, which are a different contract with a different
// owner. The numbers coincide today and the comment in store_wire.go explains at
// length why nothing may rely on that.
const (
	statusOK            uint32 = 0
	statusLimitExceeded uint32 = 3
	statusFailed        uint32 = 4
)

// Limits. The client checks these too, as a fast local failure with a better
// message -- but a limit the sender checks is a convention. These are the
// enforcement, because this module has to hold against a client that is buggy,
// old, or not aimee at all.
const (
	maxStatementBytes = 1 << 20
	maxArgs           = 4096
	maxRowsPerReply   = 4096
	maxCellBytes      = 1 << 20
	maxStatementID    = 64
)

// maxStatementTime is the longest this stage will hold a pooled connection for
// one statement, whatever the caller's deadline says.
const maxStatementTime = time.Minute

// SQLSTATEs PostgreSQL itself answers for a transaction that is not usable.
// Reported rather than invented, so the client's ErrTxClosed path keys on the
// real codes.
const (
	sqlStateNoActiveTransaction = "25P01"
	sqlStateInFailedTransaction = "25P02"
)

// txRegistry holds open transactions between calls.
//
// A transaction is a conversation across several bus round trips, so the
// connection has to outlive the call that began it. That is the whole reason
// this stage is stateful, and the reason the state is bounded and reaped:
// a caller that begins a transaction and dies would otherwise hold a pooled
// connection until the process ends.
type txRegistry struct {
	mu      sync.Mutex
	next    uint64
	open    map[uint64]*openTx
	maxOpen int
	ttl     time.Duration
}

type openTx struct {
	tx       pgx.Tx
	conn     *pgxpool.Conn
	lastUsed time.Time
}

// The ceiling and the idle timeout.
//
// maxOpenTx is below any sane pool size on purpose: reaching it means callers
// are leaking transactions, and refusing the next BEGIN says so while the pool
// still has connections for everything else. Exhausting the pool instead would
// present as the whole store hanging.
const (
	maxOpenTx = 64
	txIdleTTL = 5 * time.Minute
)

func newTxRegistry() *txRegistry {
	return &txRegistry{open: map[uint64]*openTx{}, maxOpen: maxOpenTx, ttl: txIdleTTL}
}

// add registers a live transaction, reaping idle ones first.
func (r *txRegistry) add(tx pgx.Tx, conn *pgxpool.Conn) (uint64, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.reapLocked()
	if len(r.open) >= r.maxOpen {
		return 0, fmt.Errorf("the store holds %d open transactions, its ceiling; "+
			"a caller is beginning transactions it never ends", r.maxOpen)
	}
	r.next++
	// Handle 0 is "no transaction" on the wire, so it can never name one.
	if r.next == 0 {
		r.next = 1
	}
	id := r.next
	r.open[id] = &openTx{tx: tx, conn: conn, lastUsed: time.Now()}
	return id, nil
}

func (r *txRegistry) get(id uint64) (*openTx, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	t, ok := r.open[id]
	if ok {
		t.lastUsed = time.Now()
	}
	return t, ok
}

func (r *txRegistry) remove(id uint64) (*openTx, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	t, ok := r.open[id]
	if ok {
		delete(r.open, id)
	}
	return t, ok
}

// reapLocked rolls back and releases transactions idle past the TTL.
//
// ROLLBACK, never commit. An abandoned transaction's writes were never
// confirmed to anyone, and committing them here would turn a caller's crash
// into a durable change nobody asked for.
func (r *txRegistry) reapLocked() {
	if r.ttl <= 0 {
		return
	}
	cutoff := time.Now().Add(-r.ttl)
	for id, t := range r.open {
		if t.lastUsed.After(cutoff) {
			continue
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = t.tx.Rollback(ctx)
		cancel()
		t.conn.Release()
		delete(r.open, id)
	}
}

// --- the shared pool ---------------------------------------------------------

// The SQL stage's pool, which is NOT the health probe's.
//
// Two pools in one process, on purpose and documented in both places: the probe
// keeps its own two connections precisely so it can still answer "can this
// process reach its database" when the shared pool is what is broken. A probe
// reporting on the pool it borrows reads healthy right up until it cannot
// answer at all.
var (
	sqlPoolOnce       sync.Once
	sqlPool           *pgxpool.Pool
	sqlPoolErr        error
	migrationPoolOnce sync.Once
	migrationPool     *pgxpool.Pool
	migrationPoolErr  error
)

func parseStoreConfig(dsn string) (*pgxpool.Config, error) {
	return parseStoreConfigFor("AIMEE_STORE_URL", dsn)
}

func parseStoreConfigFor(name, dsn string) (*pgxpool.Config, error) {
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		// Parse errors can contain the original DSN, including its password.
		return nil, fmt.Errorf("postgres: %s is not a valid DSN", name)
	}
	return config, nil
}

func parseMigrationConfig(migrationDSN, runtimeDSN string) (*pgxpool.Config, error) {
	config, err := parseStoreConfigFor("AIMEE_STORE_MIGRATION_URL", migrationDSN)
	if err != nil {
		return nil, err
	}
	runtime, err := parseStoreConfigFor("AIMEE_STORE_URL", runtimeDSN)
	if err != nil {
		return nil, err
	}
	if config.ConnConfig.User == runtime.ConnConfig.User {
		return nil, fmt.Errorf("postgres: migration role %q must differ from the runtime role",
			config.ConnConfig.User)
	}
	// Credentials are two capabilities on ONE database. Migrating a different
	// target succeeds deceptively while runtime keeps serving the old schema.
	// Compare configured endpoints, including failover order, without resolving
	// DNS (or printing credentials). Operators must use the same endpoint names.
	mc, rc := config.ConnConfig, runtime.ConnConfig
	if mc.Database != rc.Database || mc.Host != rc.Host || mc.Port != rc.Port || len(mc.Fallbacks) != len(rc.Fallbacks) {
		return nil, errors.New("postgres: migration and runtime DSNs must target the same database and endpoints")
	}
	for i, fallback := range mc.Fallbacks {
		if fallback.Host != rc.Fallbacks[i].Host || fallback.Port != rc.Fallbacks[i].Port {
			return nil, errors.New("postgres: migration and runtime DSNs must use the same failover endpoints")
		}
	}
	for _, key := range []string{"search_path", "options"} {
		if mc.RuntimeParams[key] != rc.RuntimeParams[key] {
			return nil, fmt.Errorf("postgres: migration and runtime DSNs must use the same %s", key)
		}
	}
	return config, nil
}

// SQLPool opens (once) the pool this stage serves from.
func SQLPool(ctx context.Context) (*pgxpool.Pool, error) {
	sqlPoolOnce.Do(func() {
		dsn := os.Getenv("AIMEE_STORE_URL")
		if dsn == "" {
			sqlPoolErr = errors.New("postgres: AIMEE_STORE_URL is unset, so the SQL " +
				"stage has no database to serve")
			return
		}
		config, err := parseStoreConfig(dsn)
		if err != nil {
			sqlPoolErr = err
			return
		}
		// Room for the transaction ceiling plus ordinary traffic. A pool smaller
		// than maxOpenTx would let leaked transactions starve every other
		// caller, which presents as the store hanging rather than as the leak
		// it is.
		if config.MaxConns < maxOpenTx+8 {
			config.MaxConns = maxOpenTx + 8
		}
		config.MinConns = 0
		pool, err := pgxpool.NewWithConfig(ctx, config)
		if err != nil {
			sqlPoolErr = fmt.Errorf("postgres: the SQL stage could not open its pool: %w", err)
			return
		}
		sqlPool, sqlPoolErr = pool, nil
	})
	return sqlPool, sqlPoolErr
}

// MigrationPool is deliberately separate from the runtime pool. Schema creation
// and version-ledger writes never travel over AIMEE_STORE_URL, so a compromise of
// the ordinary query path does not inherit DDL authority. Deployments must supply
// a distinct owner DSN and may remove it after startup migration completes.
func MigrationPool(ctx context.Context) (*pgxpool.Pool, error) {
	migrationPoolOnce.Do(func() {
		dsn := os.Getenv("AIMEE_STORE_MIGRATION_URL")
		if dsn == "" {
			migrationPoolErr = errors.New("postgres: AIMEE_STORE_MIGRATION_URL is unset")
			return
		}
		runtimeDSN := os.Getenv("AIMEE_STORE_URL")
		if runtimeDSN == "" {
			migrationPoolErr = errors.New("postgres: AIMEE_STORE_URL is unset")
			return
		}
		config, err := parseMigrationConfig(dsn, runtimeDSN)
		if err != nil {
			migrationPoolErr = err
			return
		}
		config.MaxConns = 2
		config.MinConns = 0
		pool, err := pgxpool.NewWithConfig(ctx, config)
		if err != nil {
			migrationPoolErr = fmt.Errorf("postgres: migration pool initialization failed: %w", err)
			return
		}
		if err := ensureSearchPathSchema(ctx, pool, config); err != nil {
			pool.Close()
			migrationPoolErr = err
			return
		}
		runtime, err := SQLPool(ctx)
		if err == nil {
			err = validateStoreNamespace(ctx, runtime, pool)
		}
		if err != nil {
			pool.Close()
			migrationPoolErr = err
			return
		}
		migrationPool = pool
	})
	return migrationPool, migrationPoolErr
}

// Role defaults and the implicit $user search-path entry can resolve identical
// DSN text into different schemas. Check the live resolution before any domain
// migrations, without disclosing credentials or relying on DNS aliases.
func validateStoreNamespace(ctx context.Context, runtime, migration *pgxpool.Pool) error {
	var runtimeDB, migrationDB string
	var runtimeSchemas, migrationSchemas []string
	const query = "SELECT current_database(), current_schemas(false)"
	if err := runtime.QueryRow(ctx, query).Scan(&runtimeDB, &runtimeSchemas); err != nil {
		return errors.New("postgres: cannot verify runtime database namespace")
	}
	if err := migration.QueryRow(ctx, query).Scan(&migrationDB, &migrationSchemas); err != nil {
		return errors.New("postgres: cannot verify migration database namespace")
	}
	if runtimeDB != migrationDB || len(runtimeSchemas) == 0 || !slices.Equal(runtimeSchemas, migrationSchemas) {
		return errors.New("postgres: migration and runtime roles resolve different database namespaces; configure matching search paths and schema usage grants")
	}
	return nil
}

// ensureSearchPathSchema creates the schema the DSN's search_path names.
//
// PostgreSQL does not create a schema because search_path mentions one: it
// resolves unqualified names against what already exists, and CREATE TABLE with
// nothing resolvable fails with 3F000, "no schema has been selected to create
// in". A caller that asks for its own schema is asking for isolation, so give
// it one rather than failing on the first write.
//
// Only the first entry, and only a plain identifier: that is the schema the
// caller means to own. A search_path listing several existing schemas is a
// resolution order, not a request to create anything, and is left alone.
func ensureSearchPathSchema(ctx context.Context, pool *pgxpool.Pool,
	config *pgxpool.Config) error {
	want := config.ConnConfig.RuntimeParams["search_path"]
	if i := strings.IndexByte(want, ','); i >= 0 {
		want = want[:i]
	}
	want = strings.TrimSpace(want)
	if want == "" || want == "public" || !plainIdentifier(want) {
		return nil
	}
	if _, err := pool.Exec(ctx, `CREATE SCHEMA IF NOT EXISTS `+want); err != nil {
		return fmt.Errorf("postgres: could not create the schema search_path names (%s): %w",
			want, err)
	}
	return nil
}

// plainIdentifier reports whether s is safe to interpolate as a schema name.
// Quoting is deliberately not attempted: a name needing it is not one this is
// willing to create on a caller's behalf.
func plainIdentifier(s string) bool {
	if len(s) > 63 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c == '_':
		case c >= 'A' && c <= 'Z':
		case c >= '0' && c <= '9' && i > 0:
		default:
			return false
		}
	}
	return s != ""
}

// sqlHandler serves the stage.
//
// The pool is resolved on FIRST USE rather than at construction, and that is
// what lets this stage be advertised unconditionally. The alternative -- open
// the pool at startup and drop the stage when there is no DSN -- makes the
// contract and the runtime disagree exactly when the database is unreachable,
// and a caller then gets CAPABILITY_ABSENT from a module that is plainly
// running. An explained failure is a better thing to hand an operator than
// looking absent.
//
// It also avoids a second exemption in TestAdvertisedStagesMatchTheContractFile.
// That guard already carries one, for roundtable-review, and an exemption list
// is where the wrong repair always sits one line from whoever meets the failure.
type sqlHandler struct {
	txs             *txRegistry
	poolFn          func(context.Context) (*pgxpool.Pool, error)
	migrationPoolFn func(context.Context) (*pgxpool.Pool, error)
}

// NewSQLHandler builds the stage's bus handler against the module's pool.
func NewSQLHandler() bus.ModuleHandler {
	return (&sqlHandler{txs: newTxRegistry(), poolFn: SQLPool,
		migrationPoolFn: MigrationPool}).handle
}

func (h *sqlHandler) handle(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageSQL {
		return nil, bus.ModuleStatusInvalidRequest
	}
	r := &reader{buf: frame}
	op, err := r.u32()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	poolFn := h.poolFn
	if op == opMigrate || op == opCurrentVersion {
		poolFn = h.migrationPoolFn
	}
	pool, err := poolFn(context.Background())
	if err != nil || pool == nil {
		// No database. Refused IN BAND with the reason rather than at the
		// transport, because a caller that can read "AIMEE_STORE_URL is unset"
		// can act on it, where a bare transport failure says only that something
		// went wrong somewhere.
		return refuse(statusFailed, "",
			fmt.Sprintf("the SQL stage has no database: %v", err)), bus.ModuleStatusOK
	}

	// The caller's remaining time bounds the statement, so a query cannot
	// outlive the request that asked for it -- clamped by this stage's own
	// ceiling, because how long a pooled connection may be held is this
	// module's decision and not the caller's.
	//
	// Remaining() rather than reading DeadlineNS directly, which was the first
	// version and was wrong in a way that would not have shown: DeadlineNS is an
	// ABSOLUTE CLOCK_MONOTONIC timestamp, not a duration, so converting it to a
	// Duration gives nanoseconds-since-boot -- always larger than the ceiling,
	// so the clamp would have quietly replaced every caller's deadline with the
	// maximum and nothing would have looked wrong.
	deadline := invocation.Remaining(maxStatementTime)
	if deadline <= 0 {
		// Already expired in flight. Refusing costs nothing; running the
		// statement would spend a connection on an answer nobody can receive.
		return refuse(statusFailed, "", "the request's deadline passed before the "+
			"store could run it"), bus.ModuleStatusOK
	}
	ctx, cancel := context.WithTimeout(context.Background(), deadline)
	defer cancel()

	switch op {
	case opExec, opQuery:
		return h.statement(ctx, pool, op, r)
	case opBegin:
		return h.begin(ctx, pool, r)
	case opCommit, opRollback:
		return h.finish(ctx, op, r)
	case opMigrate:
		return h.migrate(ctx, pool, r)
	case opCurrentVersion:
		return h.currentVersion(ctx, pool, r)
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
}

// statement runs one EXEC or QUERY, on the pool or inside a named transaction.
func (h *sqlHandler) statement(ctx context.Context, pool *pgxpool.Pool, op uint32, r *reader) ([]byte, bus.ModuleStatus) {
	statementID, err := r.str()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if len(statementID) > maxStatementID {
		return refuse(statusLimitExceeded, "",
			fmt.Sprintf("statement id of %d bytes exceeds %d",
				len(statementID), maxStatementID)), bus.ModuleStatusOK
	}
	handle, err := r.u64()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	sql, err := r.str()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if len(sql) > maxStatementBytes {
		return refuse(statusLimitExceeded, "",
				fmt.Sprintf("statement of %d bytes exceeds %d", len(sql), maxStatementBytes)),
			bus.ModuleStatusOK
	}
	args, err := r.args()
	if err != nil {
		if errors.Is(err, errTooManyArgs) {
			return refuse(statusLimitExceeded, "", err.Error()), bus.ModuleStatusOK
		}
		return nil, bus.ModuleStatusInvalidRequest
	}

	q, release, status := h.queryer(pool, handle)
	if status != nil {
		return status, bus.ModuleStatusOK
	}
	if release != nil {
		defer release()
	}

	if op == opExec {
		tag, err := q.Exec(ctx, sql, args...)
		if err != nil {
			return failure(err, statementID), bus.ModuleStatusOK
		}
		w := &writer{}
		w.header(statusOK, "", "")
		w.u64(uint64(tag.RowsAffected()))
		return w.buf, bus.ModuleStatusOK
	}

	rows, err := q.Query(ctx, sql, args...)
	if err != nil {
		return failure(err, statementID), bus.ModuleStatusOK
	}
	body, err := encodeRows(rows)
	if err != nil {
		if errors.Is(err, errResultTooLarge) {
			return refuse(statusLimitExceeded, "", err.Error()), bus.ModuleStatusOK
		}
		return failure(err, statementID), bus.ModuleStatusOK
	}
	return body, bus.ModuleStatusOK
}

// queryer picks the pool or a transaction. A handle naming no live transaction
// is answered with PostgreSQL's own 25P01, which is what makes it
// distinguishable at the client from a statement refused on its merits.
func (h *sqlHandler) queryer(pool *pgxpool.Pool, handle uint64) (interface {
	Exec(context.Context, string, ...any) (pgconn.CommandTag, error)
	Query(context.Context, string, ...any) (pgx.Rows, error)
}, func(), []byte) {
	if handle == 0 {
		return pool, nil, nil
	}
	t, ok := h.txs.get(handle)
	if !ok {
		return nil, nil, refuse(statusFailed, sqlStateNoActiveTransaction,
			fmt.Sprintf("no open transaction %d: it was committed, rolled back, "+
				"or reaped after being idle", handle))
	}
	return t.tx, nil, nil
}

func (h *sqlHandler) begin(ctx context.Context, pool *pgxpool.Pool, r *reader) ([]byte, bus.ModuleStatus) {
	// BEGIN arrives in the statement frame, so the fields are read and ignored
	// rather than assumed absent.
	if _, err := r.str(); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, err := r.u64(); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, err := r.str(); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	conn, err := pool.Acquire(ctx)
	if err != nil {
		return failure(err, "begin"), bus.ModuleStatusOK
	}
	tx, err := conn.Begin(ctx)
	if err != nil {
		conn.Release()
		return failure(err, "begin"), bus.ModuleStatusOK
	}
	handle, err := h.txs.add(tx, conn)
	if err != nil {
		_ = tx.Rollback(ctx)
		conn.Release()
		return refuse(statusFailed, "", err.Error()), bus.ModuleStatusOK
	}
	w := &writer{}
	w.header(statusOK, "", "")
	w.u64(handle)
	return w.buf, bus.ModuleStatusOK
}

// finish commits or rolls back, and releases the connection either way.
func (h *sqlHandler) finish(ctx context.Context, op uint32, r *reader) ([]byte, bus.ModuleStatus) {
	if _, err := r.str(); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	handle, err := r.u64()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	t, ok := h.txs.remove(handle)
	if !ok {
		return refuse(statusFailed, sqlStateNoActiveTransaction,
			fmt.Sprintf("no open transaction %d to finish", handle)), bus.ModuleStatusOK
	}
	// The connection goes back to the pool whatever the outcome. Leaking it on
	// a failed commit would remove one connection per failure until there were
	// none, and present as the store slowing down rather than as an error.
	defer t.conn.Release()

	if op == opCommit {
		err = t.tx.Commit(ctx)
	} else {
		err = t.tx.Rollback(ctx)
	}
	if err != nil {
		return failure(err, "finish"), bus.ModuleStatusOK
	}
	w := &writer{}
	w.header(statusOK, "", "")
	return w.buf, bus.ModuleStatusOK
}

// --- schema history ----------------------------------------------------------

// The ledger. Owned by this module because it is the store's record of what has
// been done to it, not any one client's: aimee records as "db1", and a second
// client with its own tables records under its own owner without either of them
// being able to see or renumber the other's history.
//
// NAMED schema_migrations because that is what was already written down. Nothing
// had ever created this table -- the stage that owns it did not exist -- so the
// only statement of its name was the end-to-end probe's
// "SELECT count(*) FROM schema_migrations WHERE owner='db1'", written against a
// contract rather than against a database. Choosing a different name here would
// have made that probe wrong about a table it was waiting for.
const versionTableDDL = `
CREATE TABLE IF NOT EXISTS schema_migrations (
    owner       TEXT   NOT NULL,
    version     BIGINT NOT NULL,
    checksum    TEXT   NOT NULL,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (owner, version)
);
-- Default application-table grants must not grant authority over the ledger.
-- Reconcile existing installations too, under the same bootstrap lock. Only
-- the table owner retains direct privileges; all ledger operations already use
-- the migration capability, including reads. Role membership remains a deploy
-- responsibility: runtime must not inherit the owner role.
DO $ledger_acl$
DECLARE recipient record;
BEGIN
    FOR recipient IN
        SELECT DISTINCT acl.grantee
        FROM pg_catalog.pg_class AS relation,
             LATERAL pg_catalog.aclexplode(relation.relacl) AS acl
        WHERE relation.oid = 'schema_migrations'::regclass
          AND acl.grantee <> relation.relowner
    LOOP
        EXECUTE format('REVOKE ALL ON TABLE schema_migrations FROM %s',
            CASE WHEN recipient.grantee = 0 THEN 'PUBLIC'
                 ELSE quote_ident(pg_catalog.pg_get_userbyid(recipient.grantee)) END);
    END LOOP;
END
$ledger_acl$`

// Shared with the native knowledge-schema bootstrap. A row lock cannot guard
// an empty ledger, and CREATE TABLE IF NOT EXISTS is not safe against concurrent
// catalog creation. Acquire this database-wide transaction lock before either
// DDL or history reads. Runtime queries never acquire it.
const schemaMigrationLockSQL = "SELECT pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtext('aimee:db:schema'))"

func rollbackSchemaTransaction(tx pgx.Tx) {
	// The request may have expired while waiting for the schema lock. Releasing
	// the transaction must not inherit that cancelled context or retain a lease.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = tx.Rollback(ctx)
}

func beginSchemaTransaction(ctx context.Context, pool *pgxpool.Pool) (pgx.Tx, error) {
	tx, err := pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	if _, err := tx.Exec(ctx, schemaMigrationLockSQL); err != nil {
		rollbackSchemaTransaction(tx)
		return nil, err
	}
	if _, err := tx.Exec(ctx, versionTableDDL); err != nil {
		rollbackSchemaTransaction(tx)
		return nil, err
	}
	return tx, nil
}

func (h *sqlHandler) currentVersion(ctx context.Context, pool *pgxpool.Pool, r *reader) ([]byte, bus.ModuleStatus) {
	owner, err := r.str()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	tx, err := beginSchemaTransaction(ctx, pool)
	if err != nil {
		return failure(err, "current_version"), bus.ModuleStatusOK
	}
	defer rollbackSchemaTransaction(tx)
	var version int64
	var checksum string
	err = tx.QueryRow(ctx,
		`SELECT version, checksum FROM schema_migrations
		  WHERE owner = $1 ORDER BY version DESC LIMIT 1`, owner).
		Scan(&version, &checksum)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return failure(err, "current_version"), bus.ModuleStatusOK
	}
	if err := tx.Commit(ctx); err != nil {
		return failure(err, "current_version"), bus.ModuleStatusOK
	}
	// No rows is a fresh database, which answers 0 rather than failing. That is
	// the ordinary first start, not an error.
	w := &writer{}
	w.header(statusOK, "", "")
	w.u64(uint64(version))
	w.str(checksum)
	return w.buf, bus.ModuleStatusOK
}

// migrate applies one versioned change and records it, in ONE transaction.
//
// The two must not be separable. A schema applied without its ledger row runs
// again on the next start and fails on an object that already exists; a ledger
// row without its schema silently claims work that never happened, which is the
// worse of the two because nothing ever retries it.
func (h *sqlHandler) migrate(ctx context.Context, pool *pgxpool.Pool, r *reader) ([]byte, bus.ModuleStatus) {
	owner, err := r.str()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	version, err := r.u64()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	claimed, err := r.str()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count, err := r.u32()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if count > maxArgs {
		return refuse(statusLimitExceeded, "",
			fmt.Sprintf("%d statements exceeds %d", count, maxArgs)), bus.ModuleStatusOK
	}
	statements := make([]string, 0, count)
	for i := uint32(0); i < count; i++ {
		s, err := r.str()
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if len(s) > maxStatementBytes {
			return refuse(statusLimitExceeded, "",
				fmt.Sprintf("migration statement of %d bytes exceeds %d",
					len(s), maxStatementBytes)), bus.ModuleStatusOK
		}
		statements = append(statements, s)
	}

	// RECOMPUTED, never recorded as handed over. Trusting the client's checksum
	// would defeat the check: the point is to notice when the statements changed
	// under a version that was already applied, and a client sending both would
	// send a matching pair either way.
	sum := checksumOf(statements)
	if sum != claimed {
		return refuse(statusFailed, "",
				fmt.Sprintf("migration %s v%d: the checksum does not match the statements "+
					"sent (recomputed %s, was told %s)", owner, version, sum, claimed)),
			bus.ModuleStatusOK
	}

	tx, err := beginSchemaTransaction(ctx, pool)
	if err != nil {
		return failure(err, "migrate"), bus.ModuleStatusOK
	}
	defer rollbackSchemaTransaction(tx)

	// Ordering and replay, decided inside the transaction so two modules
	// starting together cannot both pass the check and both apply.
	var applied int64
	var appliedSum string
	err = tx.QueryRow(ctx,
		`SELECT version, checksum FROM schema_migrations
		  WHERE owner = $1 ORDER BY version DESC LIMIT 1 FOR UPDATE`, owner).
		Scan(&applied, &appliedSum)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return failure(err, "migrate"), bus.ModuleStatusOK
	}

	var recordedSum string
	err = tx.QueryRow(ctx,
		`SELECT checksum FROM schema_migrations WHERE owner = $1 AND version = $2`,
		owner, int64(version)).Scan(&recordedSum)
	switch {
	case err == nil:
		// Already applied. Same statements is a replay and succeeds; different
		// statements is a rewritten migration, which is refused -- the database
		// cannot be made to match it without knowing what the old one did.
		if recordedSum != sum {
			return refuse(statusFailed, "",
				fmt.Sprintf("migration %s v%d was applied with checksum %s and now "+
					"hashes to %s: an applied migration may not be edited",
					owner, version, recordedSum, sum)), bus.ModuleStatusOK
		}
		// A replay executes no domain DDL, but bootstrap may have repaired
		// legacy ledger grants. Do not roll that repair back on success.
		if err := tx.Commit(ctx); err != nil {
			return failure(err, "migrate"), bus.ModuleStatusOK
		}
		w := &writer{}
		w.header(statusOK, "", "")
		return w.buf, bus.ModuleStatusOK
	case !errors.Is(err, pgx.ErrNoRows):
		return failure(err, "migrate"), bus.ModuleStatusOK
	}

	if int64(version) != applied+1 {
		return refuse(statusFailed, "",
			fmt.Sprintf("migration %s v%d cannot apply at version %d: the history is "+
				"append-only and has no gaps", owner, version, applied)), bus.ModuleStatusOK
	}

	for i, s := range statements {
		if _, err := tx.Exec(ctx, s); err != nil {
			return failure(fmt.Errorf("statement %d of %d: %w", i+1, len(statements), err),
				"migrate"), bus.ModuleStatusOK
		}
	}
	if _, err := tx.Exec(ctx,
		`INSERT INTO schema_migrations (owner, version, checksum) VALUES ($1, $2, $3)`,
		owner, int64(version), sum); err != nil {
		return failure(err, "migrate"), bus.ModuleStatusOK
	}
	if err := tx.Commit(ctx); err != nil {
		return failure(err, "migrate"), bus.ModuleStatusOK
	}
	w := &writer{}
	w.header(statusOK, "", "")
	return w.buf, bus.ModuleStatusOK
}

// --- replies -----------------------------------------------------------------

var errResultTooLarge = errors.New("the result set exceeds what the wire carries")
var errTooManyArgs = errors.New("too many arguments")

// refuse builds a reply the client reads as a refusal it can act on.
func refuse(status uint32, sqlstate, message string) []byte {
	w := &writer{}
	w.header(status, sqlstate, message)
	return w.buf
}

// failure turns a driver error into a reply, keeping the SQLSTATE.
//
// THE SQLSTATE IS THE POINT. A unique violation is a replay, a foreign-key
// violation is a caller error, and a dropped connection is an outage; the client
// classifies on exactly these codes, and collapsing them into "failed" is what
// the store did before this contract existed.
func failure(err error, op string) []byte {
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		return refuse(statusFailed, pgErr.Code, pgErr.Message)
	}
	return refuse(statusFailed, "", fmt.Sprintf("%s: %v", op, err))
}

// encodeRows frames a result set: width, count, then width*count typed cells.
//
// REFUSES RATHER THAN TRUNCATES over the ceiling. A caller handed a capped set
// could not tell it from a complete one and would record the cap as fact.
func encodeRows(rows pgx.Rows) ([]byte, error) {
	defer rows.Close()

	fields := rows.FieldDescriptions()
	width := len(fields)

	var cells []any
	count := 0
	for rows.Next() {
		values, err := rows.Values()
		if err != nil {
			return nil, err
		}
		count++
		if count > maxRowsPerReply {
			return nil, fmt.Errorf("%w: over %d rows", errResultTooLarge, maxRowsPerReply)
		}
		cells = append(cells, values...)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}

	w := &writer{}
	w.header(statusOK, "", "")
	w.u32(uint32(width))
	w.u32(uint32(count))
	for _, v := range cells {
		if err := w.value(v); err != nil {
			return nil, err
		}
	}
	return w.buf, nil
}

// --- framing -----------------------------------------------------------------

type writer struct{ buf []byte }

func (w *writer) u32(v uint32) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	w.buf = append(w.buf, b[:]...)
}

func (w *writer) u64(v uint64) {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], v)
	w.buf = append(w.buf, b[:]...)
}

func (w *writer) str(s string) {
	w.u32(uint32(len(s)))
	w.buf = append(w.buf, s...)
}

// header is the three fields every reply starts with, in every case including
// success. The client reads them unconditionally.
func (w *writer) header(status uint32, sqlstate, message string) {
	w.u32(status)
	w.str(sqlstate)
	w.str(message)
}

// value writes one result cell.
//
// The type set is shared with arguments, and the mapping from PostgreSQL's
// types is where a store wire usually goes wrong: a BOOLEAN sent as text reads
// as false through atoi, including every row whose default is true. Each branch
// below sends what the column actually is.
func (w *writer) value(v any) error {
	switch t := v.(type) {
	case nil:
		w.buf = append(w.buf, valNull)
	case string:
		if len(t) > maxCellBytes {
			return fmt.Errorf("%w: a cell of %d bytes", errResultTooLarge, len(t))
		}
		w.buf = append(w.buf, valText)
		w.str(t)
	case bool:
		w.buf = append(w.buf, valBool)
		if t {
			w.buf = append(w.buf, 1)
		} else {
			w.buf = append(w.buf, 0)
		}
	case int16:
		w.buf = append(w.buf, valInt)
		w.u64(uint64(int64(t)))
	case int32:
		w.buf = append(w.buf, valInt)
		w.u64(uint64(int64(t)))
	case int64:
		w.buf = append(w.buf, valInt)
		w.u64(uint64(t))
	case float32:
		w.buf = append(w.buf, valFloat)
		w.u64(math.Float64bits(float64(t)))
	case float64:
		w.buf = append(w.buf, valFloat)
		w.u64(math.Float64bits(t))
	case []byte:
		if t == nil {
			w.buf = append(w.buf, valNull)
			return nil
		}
		if len(t) > maxCellBytes {
			return fmt.Errorf("%w: a cell of %d bytes", errResultTooLarge, len(t))
		}
		w.buf = append(w.buf, valBytes)
		w.u32(uint32(len(t)))
		w.buf = append(w.buf, t...)
	case pgtype.Numeric:
		// NUMERIC arrives for money columns and for SUM() over them. The wire
		// has no decimal type, and every consumer of these columns reads them
		// into a double, so the conversion to float64 happens somewhere
		// regardless -- here, where it can be seen, rather than silently.
		if !t.Valid {
			w.buf = append(w.buf, valNull)
			return nil
		}
		f, err := t.Float64Value()
		if err != nil {
			return fmt.Errorf("the store cannot represent a NUMERIC as a float: %w", err)
		}
		w.buf = append(w.buf, valFloat)
		w.u64(math.Float64bits(f.Float64))
	case time.Time:
		// AS TEXT, in RFC3339 with nanoseconds, because the wire has no time
		// type and inventing one would need agreement from the other side. The
		// families read timestamps as strings today, so this is what they
		// expect; a time type would be a wire change, not a local decision.
		w.buf = append(w.buf, valText)
		w.str(t.Format(time.RFC3339Nano))
	case []string:
		if len(t) > maxArgs {
			return fmt.Errorf("%w: an array of %d", errResultTooLarge, len(t))
		}
		w.buf = append(w.buf, valTextArray)
		w.u32(uint32(len(t)))
		for _, s := range t {
			w.str(s)
		}
	default:
		// Deliberately an error rather than a fmt.Sprint fallback. A stringified
		// value looks like a working column until something reads it back as a
		// number, and then the wrong answer has already been stored.
		return fmt.Errorf("the store cannot send a %T over the wire", v)
	}
	return nil
}

type reader struct {
	buf []byte
	at  int
}

var errShort = errors.New("the frame ended early")

func (r *reader) u32() (uint32, error) {
	if r.at+4 > len(r.buf) {
		return 0, errShort
	}
	v := binary.LittleEndian.Uint32(r.buf[r.at:])
	r.at += 4
	return v, nil
}

func (r *reader) u64() (uint64, error) {
	if r.at+8 > len(r.buf) {
		return 0, errShort
	}
	v := binary.LittleEndian.Uint64(r.buf[r.at:])
	r.at += 8
	return v, nil
}

func (r *reader) byte1() (uint8, error) {
	if r.at+1 > len(r.buf) {
		return 0, errShort
	}
	v := r.buf[r.at]
	r.at++
	return v, nil
}

func (r *reader) str() (string, error) {
	n, err := r.u32()
	if err != nil {
		return "", err
	}
	if int(n) > maxCellBytes || r.at+int(n) > len(r.buf) {
		return "", errShort
	}
	s := string(r.buf[r.at : r.at+int(n)])
	r.at += int(n)
	return s, nil
}

func (r *reader) blob() ([]byte, error) {
	n, err := r.u32()
	if err != nil {
		return nil, err
	}
	if int(n) > maxCellBytes || r.at+int(n) > len(r.buf) {
		return nil, errShort
	}
	b := make([]byte, n)
	copy(b, r.buf[r.at:r.at+int(n)])
	r.at += int(n)
	return b, nil
}

// args reads the argument list a statement carries.
func (r *reader) args() ([]any, error) {
	n, err := r.u32()
	if err != nil {
		return nil, err
	}
	if n > maxArgs {
		return nil, fmt.Errorf("%w: %d arguments exceeds %d", errTooManyArgs, n, maxArgs)
	}
	out := make([]any, 0, n)
	for i := uint32(0); i < n; i++ {
		v, err := r.value()
		if err != nil {
			return nil, err
		}
		out = append(out, v)
	}
	return out, nil
}

// value reads one typed argument. NULL becomes a nil any, which pgx binds as
// SQL NULL -- distinct from an empty string, which these tables rely on.
func (r *reader) value() (any, error) {
	kind, err := r.byte1()
	if err != nil {
		return nil, err
	}
	switch kind {
	case valNull:
		return nil, nil
	case valText:
		return r.str()
	case valInt:
		u, err := r.u64()
		return int64(u), err
	case valFloat:
		u, err := r.u64()
		return math.Float64frombits(u), err
	case valBool:
		b, err := r.byte1()
		return b != 0, err
	case valBytes:
		return r.blob()
	case valTextArray:
		n, err := r.u32()
		if err != nil {
			return nil, err
		}
		if n > maxArgs {
			return nil, fmt.Errorf("%w: an array of %d", errTooManyArgs, n)
		}
		out := make([]string, 0, n)
		for i := uint32(0); i < n; i++ {
			s, err := r.str()
			if err != nil {
				return nil, err
			}
			out = append(out, s)
		}
		return out, nil
	default:
		return nil, fmt.Errorf("unknown value type %d", kind)
	}
}

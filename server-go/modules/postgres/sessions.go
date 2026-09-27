package postgres

// Host sessions preserve transaction and session-local tenant state for native
// domain callers. Credentials, connections, admission and cleanup remain inside
// the PostgreSQL process. A session is a bounded capability, never a DSN.
import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	EventSession uint32 = 11267
	StageSession uint32 = 3
)

// Native schema batches exceed a result cell's 1 MiB bound. The request still
// fits the 16 MiB bus frame; parameter and result cells retain their own limits.
const maxSessionStatementBytes = 8 << 20

const (
	sessionAcquire uint32 = 1
	sessionRelease uint32 = 2
	sessionExec    uint32 = 3
	sessionQuery   uint32 = 4
	sessionState   uint32 = 5
	sessionFetch   uint32 = 6
	sessionWait    uint32 = 7
	sessionDiscard uint32 = 8
	sessionStats   uint32 = 9
)

type postgresSession struct {
	lock       chan struct{}
	conn       *pgxpool.Conn
	used       time.Time
	closed     bool
	rows       pgx.Rows
	cancelRows context.CancelFunc
	pendingRow [][]byte
}
type sessionHandler struct {
	mu        sync.Mutex
	sessions  map[uint64]*postgresSession
	opening   int
	poolFn    func(context.Context) (*pgxpool.Pool, error)
	idle      time.Duration
	ctx       context.Context
	once      sync.Once
	stopped   bool
	refused   atomic.Uint64
	expired   atomic.Uint64
	discarded atomic.Uint64
}

func NewSessionHandler(ctx context.Context) bus.ModuleHandler {
	if ctx == nil {
		ctx = context.Background()
	}
	h := &sessionHandler{sessions: make(map[uint64]*postgresSession), poolFn: SQLPool, idle: 90 * time.Second, ctx: ctx}
	return h.handle
}
func (s *postgresSession) take(ctx context.Context) bool {
	select {
	case s.lock <- struct{}{}:
		return true
	case <-ctx.Done():
		return false
	}
}
func (s *postgresSession) unlock() { <-s.lock }
func (h *sessionHandler) startReaper() {
	h.once.Do(func() {
		go func() {
			ticker := time.NewTicker(15 * time.Second)
			defer ticker.Stop()
			for {
				select {
				case <-h.ctx.Done():
					h.close()
					return
				case now := <-ticker.C:
					h.reap(now)
				}
			}
		}()
	})
}
func (s *postgresSession) closeRows(abandon bool) {
	if s.rows == nil {
		return
	}
	if abandon && s.cancelRows != nil {
		s.cancelRows()
	}
	s.rows.Close()
	if s.cancelRows != nil {
		s.cancelRows()
	}
	s.rows, s.cancelRows, s.pendingRow = nil, nil, nil
}
func (h *sessionHandler) discard(s *postgresSession) {
	if s.closed {
		return
	}
	s.closed = true
	h.discarded.Add(1)
	s.closeRows(true)
	conn := s.conn.Hijack()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = conn.Close(ctx)
	// Cancellation may start asynchronous pgx socket cleanup. Do not leave its
	// resources behind when a private authority channel is closed.
	select {
	case <-conn.PgConn().CleanupDone():
	case <-ctx.Done():
	}
}
func (h *sessionHandler) reap(now time.Time) {
	h.mu.Lock()
	entries := make(map[uint64]*postgresSession, len(h.sessions))
	for id, s := range h.sessions {
		entries[id] = s
	}
	h.mu.Unlock()
	for id, s := range entries {
		select {
		case s.lock <- struct{}{}:
			if !s.closed && now.Sub(s.used) >= h.idle {
				h.mu.Lock()
				delete(h.sessions, id)
				h.mu.Unlock()
				h.expired.Add(1)
				h.discard(s)
			}
			s.unlock()
		default:
		}
	}
}
func (h *sessionHandler) close() {
	h.mu.Lock()
	h.stopped = true
	entries := h.sessions
	h.sessions = make(map[uint64]*postgresSession)
	h.mu.Unlock()
	for _, s := range entries {
		s.lock <- struct{}{}
		h.discard(s)
		s.unlock()
	}
}
func (h *sessionHandler) acquire(ctx context.Context) ([]byte, bus.ModuleStatus) {
	pool, err := h.poolFn(ctx)
	if err != nil || pool == nil {
		return refuse(statusFailed, "08001", "PostgreSQL runtime pool unavailable"), bus.ModuleStatusOK
	}
	// Leave capacity for the existing transactional Go clients and health probes.
	limit := int(pool.Config().MaxConns) / 2
	if limit < 1 {
		limit = 1
	}
	if limit > 32 {
		limit = 32
	}
	h.mu.Lock()
	if h.stopped || len(h.sessions)+h.opening >= limit {
		h.refused.Add(1)
		h.mu.Unlock()
		return refuse(statusLimitExceeded, "53300", "PostgreSQL session capacity reached"), bus.ModuleStatusOK
	}
	h.opening++
	h.mu.Unlock()
	conn, err := pool.Acquire(ctx)
	if err == nil {
		_, err = conn.Exec(ctx, "SET bytea_output = 'hex'")
		if err != nil {
			conn.Release()
		}
	}
	h.mu.Lock()
	h.opening--
	defer h.mu.Unlock()
	if err != nil {
		return refuse(statusFailed, "08001", "PostgreSQL session acquisition failed"), bus.ModuleStatusOK
	}
	if h.stopped {
		conn.Release()
		return refuse(statusFailed, "08003", "PostgreSQL session provider stopped"), bus.ModuleStatusOK
	}
	var id uint64
	for id == 0 || h.sessions[id] != nil {
		var bytes [8]byte
		if _, err := rand.Read(bytes[:]); err != nil {
			conn.Release()
			return nil, bus.ModuleStatusInternal
		}
		id = binary.LittleEndian.Uint64(bytes[:])
	}
	h.sessions[id] = &postgresSession{lock: make(chan struct{}, 1), conn: conn, used: time.Now()}
	h.startReaper()
	w := &writer{}
	w.header(statusOK, "", "")
	w.u64(id)
	return w.buf, bus.ModuleStatusOK
}
func (h *sessionHandler) release(id uint64, s *postgresSession) error {
	h.mu.Lock()
	delete(h.sessions, id)
	h.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if s.rows != nil {
		// An abandoned stream is not reusable: cancellation may invalidate the
		// protocol connection. Close it instead of attempting a pool reset.
		h.discard(s)
		return nil
	}
	conn := s.conn.Conn()
	fail := func(err error) error { h.discard(s); return err }
	if conn.PgConn().TxStatus() != 'I' {
		if _, err := conn.Exec(ctx, "ROLLBACK"); err != nil {
			return fail(err)
		}
	}
	if _, err := conn.Exec(ctx, "RESET SESSION AUTHORIZATION"); err != nil {
		return fail(err)
	}
	// Clear pgx's cache as well as PostgreSQL's prepared statements.
	if err := conn.DeallocateAll(ctx); err != nil {
		return fail(err)
	}
	if _, err := conn.Exec(ctx, "DISCARD ALL"); err != nil {
		return fail(err)
	}
	s.closed = true
	s.conn.Release()
	return nil
}
func (h *sessionHandler) handle(inv bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	// Only the embedding daemon can request host sessions. Public and module
	// principals use their existing operation contracts, never this capability.
	if inv.StageID != StageSession || inv.PrincipalRef != 0 || len(body) > int(bus.ModuleMessageMaxBody) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	r := &reader{buf: body}
	op, err := r.u32()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	id, err := r.u64()
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if op < sessionAcquire || op > sessionStats {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var sql string
	var args []any
	if op == sessionExec || op == sessionQuery {
		sql, err = r.strLimit(maxSessionStatementBytes)
		if err != nil || sql == "" || len(sql) > maxSessionStatementBytes {
			return nil, bus.ModuleStatusInvalidRequest
		}
		args, err = r.args()
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	var waitMS uint32
	if op == sessionWait {
		waitMS, err = r.u32()
		if err != nil || waitMS == 0 || waitMS > 60000 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if r.at != len(body) || (op == sessionAcquire || op == sessionStats) != (id == 0) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	remaining := inv.Remaining(maxStatementTime)
	if remaining <= 0 || inv.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	ctx, cancel := context.WithTimeout(h.ctx, remaining)
	keepContext := false
	defer func() {
		if !keepContext {
			cancel()
		}
	}()
	if op == sessionAcquire {
		return h.acquire(ctx)
	}
	if op == sessionStats {
		pool, err := h.poolFn(ctx)
		if err != nil || pool == nil {
			return refuse(statusFailed, "08001", "PostgreSQL pool unavailable"), bus.ModuleStatusOK
		}
		limit := int(pool.Config().MaxConns) / 2
		if limit < 1 {
			limit = 1
		}
		if limit > 32 {
			limit = 32
		}
		w := &writer{}
		w.header(statusOK, "", "")
		w.u32(uint32(limit))
		h.mu.Lock()
		w.u32(uint32(len(h.sessions)))
		w.u32(uint32(h.opening))
		h.mu.Unlock()
		w.u64(h.refused.Load())
		w.u64(h.expired.Load())
		w.u64(h.discarded.Load())
		return w.buf, bus.ModuleStatusOK
	}

	h.mu.Lock()
	s := h.sessions[id]
	h.mu.Unlock()
	if s == nil {
		return refuse(statusFailed, "08003", "PostgreSQL session expired or closed"), bus.ModuleStatusOK
	}
	if !s.take(ctx) {
		return nil, bus.ModuleStatusCancelled
	}
	defer s.unlock()
	if s.closed {
		return refuse(statusFailed, "08003", "PostgreSQL session expired or closed"), bus.ModuleStatusOK
	}
	defer func() { s.used = time.Now() }()
	if s.rows != nil && (op == sessionExec || op == sessionQuery || op == sessionWait) {
		return refuse(statusFailed, "55000", "PostgreSQL result must be consumed before another statement"), bus.ModuleStatusOK
	}
	w := &writer{}
	switch op {
	case sessionDiscard:
		h.mu.Lock()
		delete(h.sessions, id)
		h.mu.Unlock()
		h.discard(s)
		w.header(statusOK, "", "")
	case sessionRelease:
		if err := h.release(id, s); err != nil {
			return refuse(statusFailed, "08006", "PostgreSQL session reset failed; connection discarded"), bus.ModuleStatusOK
		}
		w.header(statusOK, "", "")
	case sessionState:
		w.header(statusOK, "", "")
		w.buf = append(w.buf, s.conn.Conn().PgConn().TxStatus())
	case sessionExec:
		execArgs := append([]any{pgx.QueryExecModeSimpleProtocol}, args...)
		tag, err := s.conn.Exec(ctx, sql, execArgs...)
		if err != nil {
			return failure(err, "session exec"), bus.ModuleStatusOK
		}
		w.header(statusOK, "", "")
		w.buf = append(w.buf, s.conn.Conn().PgConn().TxStatus())
		w.u64(uint64(tag.RowsAffected()))
	case sessionWait:
		waitContext, stop := context.WithTimeout(ctx, time.Duration(waitMS)*time.Millisecond)
		_, err := s.conn.Conn().WaitForNotification(waitContext)
		stop()
		notified := uint32(1)
		if errors.Is(err, context.DeadlineExceeded) {
			notified = 0
		} else if err != nil {
			return failure(err, "session notification"), bus.ModuleStatusOK
		}
		w.header(statusOK, "", "")
		w.u32(notified)
	case sessionQuery, sessionFetch:
		if op == sessionQuery {
			queryArgs := append([]any{pgx.QueryExecModeSimpleProtocol}, args...)
			rows, err := s.conn.Query(ctx, sql, queryArgs...)
			if err != nil {
				return failure(err, "session query"), bus.ModuleStatusOK
			}
			s.rows, s.cancelRows = rows, cancel
		} else if s.rows == nil {
			return refuse(statusFailed, "55000", "PostgreSQL session has no pending result"), bus.ModuleStatusOK
		}
		result, more, err := s.resultPage()
		if err != nil {
			s.closeRows(true)
			if errors.Is(err, errResultTooLarge) {
				return refuse(statusLimitExceeded, "54000", "PostgreSQL row exceeds session reply bounds"), bus.ModuleStatusOK
			}
			return failure(err, "session query"), bus.ModuleStatusOK
		}
		if more {
			keepContext = op == sessionQuery
		} else {
			s.closeRows(false)
		}
		w.header(statusOK, "", "")
		w.buf = append(w.buf, s.conn.Conn().PgConn().TxStatus())
		w.buf = append(w.buf, result...)

	}
	return w.buf, bus.ModuleStatusOK
}

// Pages preserve exact PostgreSQL text and the original query snapshot. The
// native client consumes every page before returning a result, so nested domain
// statements do not compete with an unread pgx cursor. No rows are truncated and
// no domain data is spooled to the application filesystem.
func (s *postgresSession) resultPage() ([]byte, bool, error) {
	fields := s.rows.FieldDescriptions()
	if len(fields) > maxArgs {
		return nil, false, errResultTooLarge
	}
	metadata := &writer{}
	for _, field := range fields {
		if len(field.Name) > 1024 {
			return nil, false, errResultTooLarge
		}
		metadata.str(field.Name)
		metadata.u32(field.DataTypeOID)
	}
	budget := int(bus.ModuleMessageMaxBody) - len(metadata.buf) - 64
	cells := &writer{}
	count, more := 0, false
	for count < maxRowsPerReply {
		values := s.pendingRow
		if values == nil {
			if !s.rows.Next() {
				break
			}
			values = s.rows.RawValues()
		}
		rowSize := 0
		for _, value := range values {
			if len(value) > maxCellBytes {
				return nil, false, errResultTooLarge
			}
			rowSize += 4 + len(value)
		}
		if rowSize > budget {
			return nil, false, errResultTooLarge
		}
		if len(cells.buf)+rowSize > budget {
			// RawValues belongs to pgx. Retain exactly one row across requests.
			s.pendingRow = make([][]byte, len(values))
			for i, v := range values {
				if v != nil {
					s.pendingRow[i] = append([]byte{}, v...)
				}
			}
			more = true
			break
		}
		s.pendingRow = nil
		for _, value := range values {
			if value == nil {
				cells.u32(^uint32(0))
				continue
			}
			cells.u32(uint32(len(value)))
			cells.buf = append(cells.buf, value...)
		}
		count++
	}
	if err := s.rows.Err(); err != nil {
		return nil, false, err
	}
	if count == maxRowsPerReply {
		more = true
	}
	w := &writer{}
	if more {
		w.buf = append(w.buf, 1)
	} else {
		w.buf = append(w.buf, 0)
	}
	w.u64(uint64(s.rows.CommandTag().RowsAffected()))
	w.u32(uint32(len(fields)))
	w.u32(uint32(count))
	w.buf = append(w.buf, metadata.buf...)
	w.buf = append(w.buf, cells.buf...)
	return w.buf, more, nil
}

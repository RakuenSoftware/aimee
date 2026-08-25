package postgres

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	protocol "github.com/JBailes/aimee/server-go/db3"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

const (
	db3ClaimLimit       = 32
	db3BackfillLimit    = 128
	db3LeaseDuration    = 30 * time.Second
	db3AckRetryDelay    = 5 * time.Second
	db3IdlePollInterval = 250 * time.Millisecond
	db3BackfillRetryMax = 30 * time.Second
)

var (
	ErrDB3OutboxConfig        = errors.New("db3 outbox: invalid configuration")
	ErrDB3CorpusGeneration    = errors.New("db3 outbox: corpus generation conflict")
	ErrDB3ProviderNotCaughtUp = errors.New("db3 outbox: provider backfill is not acknowledged")
	ErrDB3UnknownAppliedAck   = errors.New("db3 outbox: unknown applied acknowledgement")
	ErrDB3MalformedRow        = errors.New("db3 outbox: malformed durable operation")
)

type db3SQL interface {
	Exec(context.Context, string, ...any) (pgconn.CommandTag, error)
	Query(context.Context, string, ...any) (pgx.Rows, error)
	QueryRow(context.Context, string, ...any) pgx.Row
}

// PGDB3Outbox is DB2's PostgreSQL-backed delivery ledger. It stores no DB
// handles in the wire protocol and treats a bus publish only as an attempt;
// authenticated Applied notifications are the durable completion evidence.
type PGDB3Outbox struct {
	db         db3SQL
	leaseOwner string
	backfillMu sync.Mutex
	backfills  map[uint32]struct{}
	// backfillErrors retains the last worker failure per principal until a new
	// worker starts. Operators can therefore distinguish an idle completed
	// provider from one whose retry loop encountered or stopped on an error.
	backfillErrors map[uint32]error
	backfillRetry  time.Duration
}

func NewPGDB3Outbox(db db3SQL, leaseOwner string) (*PGDB3Outbox, error) {
	if db == nil || len(leaseOwner) == 0 || len(leaseOwner) > 64 {
		return nil, ErrDB3OutboxConfig
	}
	for index := range len(leaseOwner) {
		if leaseOwner[index] < 0x21 || leaseOwner[index] > 0x7e {
			return nil, ErrDB3OutboxConfig
		}
	}
	return &PGDB3Outbox{
		db: db, leaseOwner: leaseOwner, backfills: make(map[uint32]struct{}),
		backfillErrors: make(map[uint32]error), backfillRetry: db3IdlePollInterval,
	}, nil
}

const db3AdmitProviderSQL = `SELECT public.db3_admit_provider($1,$2,$3,$4,$5)`

// AdmitProvider durably registers one authenticated apply-capable principal.
// All active providers share one corpus epoch because Apply is broadcast.
func (store *PGDB3Outbox) AdmitProvider(ctx context.Context, principal, handle uint32,
	sequence uint64, capabilities protocol.Capabilities) error {
	if store == nil || principal == 0 || sequence == 0 ||
		capabilities.Validate() != nil ||
		capabilities.Operations&protocol.OperationApply == 0 ||
		capabilities.Generation == 0 {
		return ErrDB3OutboxConfig
	}
	var result int
	if err := store.db.QueryRow(ctx, db3AdmitProviderSQL, principal, capabilities.Generation,
		handle, sequence, capabilities.Ready).Scan(&result); err != nil {
		return err
	}
	if result == 1 {
		return ErrDB3ProviderNotCaughtUp
	}
	if result != 2 {
		return ErrDB3CorpusGeneration
	}
	return nil
}

const db3BackfillChunkSQL = `SELECT public.db3_backfill_provider_chunk($1,$2)`

// BackfillProvider advances one catalog projection at a time. Each call is a
// separate PostgreSQL transaction and the SQL owner caps it at 256 rows, so the
// live-capture advisory lock is released between bounded chunks. Reattachment
// resumes the durable cursor rows left by cancellation or process failure.
func (store *PGDB3Outbox) BackfillProvider(ctx context.Context, principal uint32) error {
	if store == nil || principal == 0 {
		return ErrDB3OutboxConfig
	}
	for {
		var result int
		if err := store.db.QueryRow(ctx, db3BackfillChunkSQL,
			principal, db3BackfillLimit).Scan(&result); err != nil {
			return err
		}
		switch result {
		case 0:
			return nil
		case 1:
			continue
		default:
			return ErrDB3OutboxConfig
		}
	}
}

// LastBackfillError reports the last observed asynchronous backfill failure.
// A new admission attempt clears the previous observation before it starts its
// worker. The returned error is diagnostic; provider state in PostgreSQL stays
// authoritative for readiness.
func (store *PGDB3Outbox) LastBackfillError(principal uint32) error {
	if store == nil || principal == 0 {
		return ErrDB3OutboxConfig
	}
	store.backfillMu.Lock()
	defer store.backfillMu.Unlock()
	return store.backfillErrors[principal]
}

func retryDB3Backfill(err error) bool {
	if err == nil || errors.Is(err, context.Canceled) ||
		errors.Is(err, context.DeadlineExceeded) || errors.Is(err, ErrDB3OutboxConfig) {
		return false
	}
	var postgresError *pgconn.PgError
	if !errors.As(err, &postgresError) {
		// Driver, transport, and pool errors do not reliably expose SQLSTATE.
		// Retry them; cancellation remains the bounded shutdown mechanism.
		return true
	}
	if len(postgresError.Code) < 2 {
		return false
	}
	switch postgresError.Code[:2] {
	case "08", // connection exception
		"40", // transaction rollback / serialization
		"53", // insufficient resources
		"55", // object not in prerequisite state (for example failover)
		"57", // operator intervention
		"58": // system error
		return true
	default:
		return false
	}
}

func (store *PGDB3Outbox) runBackfillWorker(ctx context.Context, principal uint32) error {
	delay := store.backfillRetry
	if delay <= 0 {
		delay = db3IdlePollInterval
	}
	for {
		err := store.BackfillProvider(ctx, principal)
		if err == nil || !retryDB3Backfill(err) {
			return err
		}
		store.backfillMu.Lock()
		store.backfillErrors[principal] = err
		store.backfillMu.Unlock()
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return ctx.Err()
		case <-timer.C:
		}
		if delay < db3BackfillRetryMax/2 {
			delay *= 2
		} else {
			delay = db3BackfillRetryMax
		}
	}
}

func (store *PGDB3Outbox) startBackfill(ctx context.Context, principal uint32) {
	store.backfillMu.Lock()
	if _, exists := store.backfills[principal]; exists {
		store.backfillMu.Unlock()
		return
	}
	store.backfills[principal] = struct{}{}
	delete(store.backfillErrors, principal)
	store.backfillMu.Unlock()
	go func() {
		err := store.runBackfillWorker(ctx, principal)
		defer func() {
			store.backfillMu.Lock()
			delete(store.backfills, principal)
			if err != nil {
				store.backfillErrors[principal] = err
			}
			store.backfillMu.Unlock()
		}()
	}()
}

const db3ClaimSQL = `
WITH candidates AS (
  SELECT o.operation_id
    FROM db3_outbox o
   WHERE o.next_attempt_at<=pg_catalog.clock_timestamp()
     AND (o.lease_until IS NULL OR o.lease_until<pg_catalog.clock_timestamp())
     AND EXISTS (
       SELECT 1
         FROM db3_delivery d JOIN db3_provider p USING(principal)
        WHERE d.operation_id=o.operation_id AND d.state='pending' AND p.state='active'
     )
   ORDER BY o.operation_id
   LIMIT $1
   FOR UPDATE SKIP LOCKED
), leased AS (
  UPDATE db3_outbox o SET
    lease_owner=$2,
    lease_until=pg_catalog.clock_timestamp()+($3*interval '1 millisecond'),
    last_error=''
  FROM candidates c
  WHERE o.operation_id=c.operation_id
  RETURNING o.operation_id,o.corpus_generation,o.point_id,o.operation_kind,
            o.collection,o.vector_text,o.labels
)
SELECT operation_id,corpus_generation,point_id,operation_kind,collection,vector_text,labels
  FROM leased ORDER BY operation_id`

func parseDB3Vector(value string) ([]float32, error) {
	if len(value) < 2 || value[0] != '[' || value[len(value)-1] != ']' {
		return nil, ErrDB3MalformedRow
	}
	body := value[1 : len(value)-1]
	if body == "" {
		return nil, ErrDB3MalformedRow
	}
	parts := strings.Split(body, ",")
	if len(parts) > protocol.MaxDimension {
		return nil, ErrDB3MalformedRow
	}
	vector := make([]float32, len(parts))
	for index, part := range parts {
		parsed, err := strconv.ParseFloat(strings.TrimSpace(part), 32)
		if err != nil || math.IsNaN(parsed) || math.IsInf(parsed, 0) {
			return nil, ErrDB3MalformedRow
		}
		vector[index] = float32(parsed)
	}
	return vector, nil
}

func parseDB3Labels(raw []byte) ([]protocol.ExactLabel, error) {
	var labels map[string]string
	if err := json.Unmarshal(raw, &labels); err != nil || len(labels) == 0 ||
		len(labels) > protocol.MaxLabelCount {
		return nil, ErrDB3MalformedRow
	}
	keys := make([]string, 0, len(labels))
	for key := range labels {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	result := make([]protocol.ExactLabel, 0, len(keys))
	for _, key := range keys {
		result = append(result, protocol.ExactLabel{Key: key, Value: labels[key]})
	}
	return result, nil
}

func scanDB3Apply(rows pgx.Rows) (protocol.Apply, error) {
	var apply protocol.Apply
	var kind, vectorText string
	var labelsJSON []byte
	if err := rows.Scan(&apply.OperationID, &apply.Generation, &apply.PointID, &kind,
		&apply.Collection, &vectorText, &labelsJSON); err != nil {
		return protocol.Apply{}, err
	}
	switch kind {
	case "upsert":
		apply.Kind = protocol.ApplyUpsert
		vector, err := parseDB3Vector(vectorText)
		if err != nil {
			return protocol.Apply{}, err
		}
		apply.Vector = vector
		labels, err := parseDB3Labels(labelsJSON)
		if err != nil {
			return protocol.Apply{}, err
		}
		apply.Labels = labels
	case "delete":
		apply.Kind = protocol.ApplyDelete
	case "tombstone":
		apply.Kind = protocol.ApplyTombstone
	default:
		return protocol.Apply{}, ErrDB3MalformedRow
	}
	if apply.Validate() != nil {
		return protocol.Apply{}, ErrDB3MalformedRow
	}
	return apply, nil
}

func (store *PGDB3Outbox) Claim(ctx context.Context, limit int) ([]protocol.Apply, error) {
	if store == nil || limit <= 0 || limit > db3ClaimLimit {
		return nil, ErrDB3OutboxConfig
	}
	rows, err := store.db.Query(ctx, db3ClaimSQL, limit, store.leaseOwner,
		db3LeaseDuration.Milliseconds())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	operations := make([]protocol.Apply, 0, limit)
	for rows.Next() {
		apply, err := scanDB3Apply(rows)
		if err != nil {
			return nil, err
		}
		operations = append(operations, apply)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return operations, nil
}

const db3PublishedSQL = `
WITH released AS (
  UPDATE db3_outbox SET
    publish_attempts=publish_attempts+1,
    next_attempt_at=pg_catalog.clock_timestamp()+($3*interval '1 millisecond'),
    lease_owner='',lease_until=NULL,last_error='',
    published_at=pg_catalog.clock_timestamp()
  WHERE operation_id=$1 AND lease_owner=$2
  RETURNING operation_id
)
UPDATE db3_delivery d SET attempts=attempts+1,updated_at=pg_catalog.clock_timestamp()
 FROM released r,db3_provider p
 WHERE d.operation_id=r.operation_id AND d.principal=p.principal
   AND d.state='pending' AND p.state='active'`

func (store *PGDB3Outbox) Published(ctx context.Context, operationID uint64) error {
	if store == nil || operationID == 0 {
		return ErrDB3OutboxConfig
	}
	_, err := store.db.Exec(ctx, db3PublishedSQL, operationID, store.leaseOwner,
		db3AckRetryDelay.Milliseconds())
	return err
}

const db3ReleaseSQL = `
UPDATE db3_outbox SET lease_owner='',lease_until=NULL,last_error=$3,
  next_attempt_at=pg_catalog.clock_timestamp()+($4*interval '1 millisecond')
 WHERE operation_id=$1 AND lease_owner=$2`

func (store *PGDB3Outbox) Release(ctx context.Context, operationID uint64, cause error) error {
	if store == nil || operationID == 0 {
		return ErrDB3OutboxConfig
	}
	message := "publish failed"
	if cause != nil {
		message = cause.Error()
	}
	if len(message) > 256 {
		message = message[:256]
	}
	_, err := store.db.Exec(ctx, db3ReleaseSQL, operationID, store.leaseOwner, message,
		db3AckRetryDelay.Milliseconds())
	return err
}

const db3AppliedSQL = `
WITH recorded AS (
  UPDATE db3_delivery d SET
    state=CASE $4
      WHEN 0 THEN 'acked'
      WHEN 2 THEN 'quarantined'
      ELSE 'pending'
    END,
    watermark=GREATEST(d.watermark,$5),
    last_result=$4,
    updated_at=pg_catalog.clock_timestamp()
  FROM db3_outbox o,db3_provider p
  WHERE d.operation_id=$2 AND d.principal=$1
    AND o.operation_id=d.operation_id AND o.corpus_generation=$3
    AND p.principal=d.principal AND p.state='active'
  RETURNING d.operation_id,d.state
), provider_mark AS (
  UPDATE db3_provider p SET
    watermark=GREATEST(p.watermark,$5),
    last_seen_at=pg_catalog.clock_timestamp()
  WHERE p.principal=$1 AND $4=0 AND EXISTS(SELECT 1 FROM recorded)
  RETURNING 1
), cleaned AS (
  DELETE FROM db3_outbox o
   WHERE o.operation_id=$2 AND EXISTS(SELECT 1 FROM recorded)
     AND NOT EXISTS (
       SELECT 1 FROM db3_delivery d JOIN db3_provider p USING(principal)
        WHERE d.operation_id=o.operation_id AND p.state='active' AND d.state<>'acked'
     )
  RETURNING 1
)
SELECT EXISTS(SELECT 1 FROM recorded)`

func (store *PGDB3Outbox) Applied(ctx context.Context, principal uint32,
	applied protocol.Applied) error {
	if store == nil || principal == 0 || applied.Validate() != nil {
		return ErrDB3OutboxConfig
	}
	var recorded bool
	if err := store.db.QueryRow(ctx, db3AppliedSQL, principal, applied.OperationID,
		applied.Generation, applied.Result, applied.Watermark).Scan(&recorded); err != nil {
		return err
	}
	if !recorded {
		return ErrDB3UnknownAppliedAck
	}
	return nil
}

const db3RetireSQL = `
WITH retired AS (
  UPDATE db3_provider SET state='retired',retired_at=pg_catalog.clock_timestamp()
   WHERE principal=$1 AND state='active'
  RETURNING 1
)
DELETE FROM db3_outbox o
 WHERE EXISTS(SELECT 1 FROM retired)
   AND NOT EXISTS (
     SELECT 1 FROM db3_delivery d JOIN db3_provider p USING(principal)
      WHERE d.operation_id=o.operation_id AND p.state='active' AND d.state<>'acked'
   )`

func (store *PGDB3Outbox) RetireProvider(ctx context.Context, principal uint32) error {
	if store == nil || principal == 0 {
		return ErrDB3OutboxConfig
	}
	_, err := store.db.Exec(ctx, db3RetireSQL, principal)
	return err
}

// AppliedObserver is the handler for a provider's acknowledgements.
//
// An Applied notification is the DURABLE completion evidence: a bus publish is
// only an attempt, and an operation is not done until the provider says it
// landed. Everything else about delivery is a retry.
//
// The capability half of this used to live here too -- a provider was admitted
// by announcing itself, and refused if it had not caught up. That is gone with
// discovery: a provider is provisioned by grant, known at boot, and cannot
// admit or unadmit itself at runtime. What remains is the watermark, which is a
// fact about replication rather than about who is allowed to serve.
func (store *PGDB3Outbox) AppliedObserver(ctx context.Context) func(uint32, protocol.Applied) {
	if ctx == nil {
		ctx = context.Background()
	}
	return func(principal uint32, applied protocol.Applied) {
		_ = store.Applied(ctx, principal, applied)
	}
}

type db3OutboxStore interface {
	Claim(context.Context, int) ([]protocol.Apply, error)
	Published(context.Context, uint64) error
	Release(context.Context, uint64, error) error
}

type db3ApplyPublisher interface {
	PublishApply(context.Context, protocol.Apply) error
}

// RunDB3Outbox replays committed operations until cancellation. Multiple
// dispatchers are safe because Claim uses PostgreSQL SKIP LOCKED leases.
func RunDB3Outbox(ctx context.Context, store db3OutboxStore, publisher db3ApplyPublisher) error {
	if ctx == nil || store == nil || publisher == nil {
		return ErrDB3OutboxConfig
	}
	timer := time.NewTimer(0)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-timer.C:
		}
		operations, err := store.Claim(ctx, db3ClaimLimit)
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			timer.Reset(db3AckRetryDelay)
			continue
		}
		for _, apply := range operations {
			if err := publisher.PublishApply(ctx, apply); err != nil {
				_ = store.Release(ctx, apply.OperationID, err)
				if ctx.Err() != nil {
					return ctx.Err()
				}
				continue
			}
			if err := store.Published(ctx, apply.OperationID); err != nil {
				// Publishing is not durable completion evidence. If recording the
				// attempt fails, release the lease so this operation is replayed;
				// Apply is idempotent by operation_id and an authenticated Applied
				// notification remains the only acknowledgement that can delete it.
				_ = store.Release(ctx, apply.OperationID, err)
				if ctx.Err() != nil {
					return ctx.Err()
				}
			}
		}
		if len(operations) == db3ClaimLimit {
			timer.Reset(0)
		} else {
			timer.Reset(db3IdlePollInterval)
		}
	}
}

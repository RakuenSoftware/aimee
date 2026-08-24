package db2

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

	protocol "github.com/JBailes/aimee/server-go/vector"
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
	ErrVectorOutboxConfig        = errors.New("vector outbox: invalid configuration")
	ErrVectorCorpusGeneration    = errors.New("vector outbox: corpus generation conflict")
	ErrVectorProviderNotCaughtUp = errors.New("vector outbox: provider backfill is not acknowledged")
	ErrVectorUnknownAppliedAck   = errors.New("vector outbox: unknown applied acknowledgement")
	ErrVectorMalformedRow        = errors.New("vector outbox: malformed durable operation")
)

type db3SQL interface {
	Exec(context.Context, string, ...any) (pgconn.CommandTag, error)
	Query(context.Context, string, ...any) (pgx.Rows, error)
	QueryRow(context.Context, string, ...any) pgx.Row
}

// PGVectorOutbox is DB2's PostgreSQL-backed delivery ledger. It stores no DB
// handles in the wire protocol and treats a bus publish only as an attempt;
// authenticated Applied notifications are the durable completion evidence.
type PGVectorOutbox struct {
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

func NewPGVectorOutbox(db db3SQL, leaseOwner string) (*PGVectorOutbox, error) {
	if db == nil || len(leaseOwner) == 0 || len(leaseOwner) > 64 {
		return nil, ErrVectorOutboxConfig
	}
	for index := range len(leaseOwner) {
		if leaseOwner[index] < 0x21 || leaseOwner[index] > 0x7e {
			return nil, ErrVectorOutboxConfig
		}
	}
	return &PGVectorOutbox{
		db: db, leaseOwner: leaseOwner, backfills: make(map[uint32]struct{}),
		backfillErrors: make(map[uint32]error), backfillRetry: db3IdlePollInterval,
	}, nil
}

const db3AdmitProviderSQL = `SELECT public.db3_admit_provider($1,$2,$3,$4,$5)`

// AdmitProvider durably registers one authenticated apply-capable principal.
// All active providers share one corpus epoch because Apply is broadcast.
func (store *PGVectorOutbox) AdmitProvider(ctx context.Context, principal, handle uint32,
	sequence uint64, capabilities protocol.Capabilities) error {
	if store == nil || principal == 0 || sequence == 0 ||
		capabilities.Validate() != nil ||
		capabilities.Operations&protocol.OperationApply == 0 ||
		capabilities.Generation == 0 {
		return ErrVectorOutboxConfig
	}
	var result int
	if err := store.db.QueryRow(ctx, db3AdmitProviderSQL, principal, capabilities.Generation,
		handle, sequence, capabilities.Ready).Scan(&result); err != nil {
		return err
	}
	if result == 1 {
		return ErrVectorProviderNotCaughtUp
	}
	if result != 2 {
		return ErrVectorCorpusGeneration
	}
	return nil
}

const db3BackfillChunkSQL = `SELECT public.db3_backfill_provider_chunk($1,$2)`

// BackfillProvider advances one catalog projection at a time. Each call is a
// separate PostgreSQL transaction and the SQL owner caps it at 256 rows, so the
// live-capture advisory lock is released between bounded chunks. Reattachment
// resumes the durable cursor rows left by cancellation or process failure.
func (store *PGVectorOutbox) BackfillProvider(ctx context.Context, principal uint32) error {
	if store == nil || principal == 0 {
		return ErrVectorOutboxConfig
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
			return ErrVectorOutboxConfig
		}
	}
}

// LastBackfillError reports the last observed asynchronous backfill failure.
// A new admission attempt clears the previous observation before it starts its
// worker. The returned error is diagnostic; provider state in PostgreSQL stays
// authoritative for readiness.
func (store *PGVectorOutbox) LastBackfillError(principal uint32) error {
	if store == nil || principal == 0 {
		return ErrVectorOutboxConfig
	}
	store.backfillMu.Lock()
	defer store.backfillMu.Unlock()
	return store.backfillErrors[principal]
}

func retryDB3Backfill(err error) bool {
	if err == nil || errors.Is(err, context.Canceled) ||
		errors.Is(err, context.DeadlineExceeded) || errors.Is(err, ErrVectorOutboxConfig) {
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

func (store *PGVectorOutbox) runBackfillWorker(ctx context.Context, principal uint32) error {
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

func (store *PGVectorOutbox) startBackfill(ctx context.Context, principal uint32) {
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
		return nil, ErrVectorMalformedRow
	}
	body := value[1 : len(value)-1]
	if body == "" {
		return nil, ErrVectorMalformedRow
	}
	parts := strings.Split(body, ",")
	if len(parts) > protocol.MaxDimension {
		return nil, ErrVectorMalformedRow
	}
	vector := make([]float32, len(parts))
	for index, part := range parts {
		parsed, err := strconv.ParseFloat(strings.TrimSpace(part), 32)
		if err != nil || math.IsNaN(parsed) || math.IsInf(parsed, 0) {
			return nil, ErrVectorMalformedRow
		}
		vector[index] = float32(parsed)
	}
	return vector, nil
}

// A label value is a string, or an array of strings for a multi-valued label.
//
// map[string]string could not express the second, which is the form scope
// visibility needs: a point carries every scope it belongs to under one key, so
// the four-way disjunction becomes one FilterIn on the wire. json.RawMessage per
// key rather than `any` so a number or an object is a decode failure here rather
// than something that stringifies into a plausible label further on.
//
// The count is of PAIRS. A key with four values is four labels' worth of wire,
// and the codec bounds labels the same way.
func parseDB3Labels(raw []byte) ([]protocol.ExactLabel, error) {
	var labels map[string]json.RawMessage
	if err := json.Unmarshal(raw, &labels); err != nil || len(labels) == 0 {
		return nil, ErrVectorMalformedRow
	}
	keys := make([]string, 0, len(labels))
	for key := range labels {
		keys = append(keys, key)
	}
	sort.Strings(keys)

	result := make([]protocol.ExactLabel, 0, len(keys))
	for _, key := range keys {
		var single string
		if err := json.Unmarshal(labels[key], &single); err == nil {
			result = append(result, protocol.ExactLabel{Key: key, Value: single})
			continue
		}
		var many []string
		if err := json.Unmarshal(labels[key], &many); err != nil || len(many) == 0 {
			return nil, ErrVectorMalformedRow
		}
		// Sorted within the key, because the codec requires labels strictly
		// ascending by (key, value) and the database's array order is the order
		// the projection produced, not a sorted one.
		sort.Strings(many)
		for _, value := range many {
			result = append(result, protocol.ExactLabel{Key: key, Value: value})
		}
	}
	if len(result) > protocol.MaxLabelCount {
		return nil, ErrVectorMalformedRow
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
		return protocol.Apply{}, ErrVectorMalformedRow
	}
	if apply.Validate() != nil {
		return protocol.Apply{}, ErrVectorMalformedRow
	}
	return apply, nil
}

func (store *PGVectorOutbox) Claim(ctx context.Context, limit int) ([]protocol.Apply, error) {
	if store == nil || limit <= 0 || limit > db3ClaimLimit {
		return nil, ErrVectorOutboxConfig
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

func (store *PGVectorOutbox) Published(ctx context.Context, operationID uint64) error {
	if store == nil || operationID == 0 {
		return ErrVectorOutboxConfig
	}
	_, err := store.db.Exec(ctx, db3PublishedSQL, operationID, store.leaseOwner,
		db3AckRetryDelay.Milliseconds())
	return err
}

const db3ReleaseSQL = `
UPDATE db3_outbox SET lease_owner='',lease_until=NULL,last_error=$3,
  next_attempt_at=pg_catalog.clock_timestamp()+($4*interval '1 millisecond')
 WHERE operation_id=$1 AND lease_owner=$2`

func (store *PGVectorOutbox) Release(ctx context.Context, operationID uint64, cause error) error {
	if store == nil || operationID == 0 {
		return ErrVectorOutboxConfig
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

func (store *PGVectorOutbox) Applied(ctx context.Context, principal uint32,
	applied protocol.Applied) error {
	if store == nil || principal == 0 || applied.Validate() != nil {
		return ErrVectorOutboxConfig
	}
	var recorded bool
	if err := store.db.QueryRow(ctx, db3AppliedSQL, principal, applied.OperationID,
		applied.Generation, applied.Result, applied.Watermark).Scan(&recorded); err != nil {
		return err
	}
	if !recorded {
		return ErrVectorUnknownAppliedAck
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

func (store *PGVectorOutbox) RetireProvider(ctx context.Context, principal uint32) error {
	if store == nil || principal == 0 {
		return ErrVectorOutboxConfig
	}
	_, err := store.db.Exec(ctx, db3RetireSQL, principal)
	return err
}

// BusObservers binds the durable ledger to authenticated bus evidence. A
// failed admission keeps that provider out of search routing; a transient ack
// write failure is repaired when the leased operation is replayed.
func (store *PGVectorOutbox) BusObservers(ctx context.Context) DB3BusObservers {
	if ctx == nil {
		ctx = context.Background()
	}
	return DB3BusObservers{
		Capabilities: func(callCtx context.Context, principal, handle uint32, sequence uint64,
			capabilities protocol.Capabilities) error {
			err := store.AdmitProvider(callCtx, principal, handle, sequence, capabilities)
			if errors.Is(err, ErrVectorProviderNotCaughtUp) {
				store.startBackfill(ctx, principal)
			}
			return err
		},
		Applied: func(principal uint32, applied protocol.Applied) {
			_ = store.Applied(ctx, principal, applied)
		},
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

// RunVectorOutbox replays committed operations until cancellation. Multiple
// dispatchers are safe because Claim uses PostgreSQL SKIP LOCKED leases.
func RunVectorOutbox(ctx context.Context, store db3OutboxStore, publisher db3ApplyPublisher) error {
	if ctx == nil || store == nil || publisher == nil {
		return ErrVectorOutboxConfig
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

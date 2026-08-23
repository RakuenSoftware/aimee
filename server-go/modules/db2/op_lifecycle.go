package db2

import (
	"context"
	"errors"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageEmbeddingDimension,
		db2contract.OperationEmbeddingDimension, embeddingDimension)
	Register(db2contract.StagePoolStatus,
		db2contract.OperationPoolStatus, poolStatus)
	Register(db2contract.StageEmbeddingRefusals,
		db2contract.OperationEmbeddingRefusals, embeddingRefusals)
	Register(db2contract.StagePostgresStatus,
		db2contract.OperationPostgresStatus, postgresStatus)
	Register(db2contract.StageReembedStatus,
		db2contract.OperationReembedStatus, reembedStatus)
	Register(db2contract.StageReembedClear,
		db2contract.OperationReembedClear, reembedClear)
	Register(db2contract.StageReembedClearMaintenance,
		db2contract.OperationReembedClearMaintenance, reembedClearMaintenance)
	Register(db2contract.StageEmbedderServingID,
		db2contract.OperationEmbedderServingID, embedderServingID)
	Register(db2contract.StageDimensionReset,
		db2contract.OperationDimensionReset, dimensionReset)
}

// Several of these answer about the process rather than about the data, and
// this module is a different process from the C one.
//
// The pool counters, the refusal counters and the embedder identity all
// describe whoever is holding the connections. Answered here they describe this
// module's pool and this module's configuration -- which is the right answer
// once this module owns them, and a different answer from the C's until it
// does. The parity harness records that as an accepted divergence rather than
// pretending the two can agree.
const reembedInProgressKey = "reembed_in_progress"

const healthProbeQuery = `SELECT
 EXISTS (SELECT 1 FROM information_schema.tables
   WHERE table_schema = current_schema() AND table_name = 'memories'),
 EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'pg_trgm'),
 (SELECT COUNT(*) = 2 FROM information_schema.tables
   WHERE table_schema = current_schema()
     AND table_name IN ('kb_documents', 'kb_async_jobs'))`

// health answers whether the store is ready to be used.
//
// Not registered as a generic operation: its envelope is db2-health-v1, with
// its own magic and no operation field, so the dispatcher's generic header
// decode cannot read it. The dispatcher routes the health stage here before
// that decode -- see NewDispatchHandler.
//
// Three separate facts rather than one verdict: the schema being applied, the
// trigram extension being present, and the knowledge tables existing are
// different failures with different fixes, and a single boolean would make them
// indistinguishable.
func health(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeHealthRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var evidence db2contract.HealthEvidence
	if err := store.QueryRow(ctx, healthProbeQuery).Scan(&evidence.SchemaOK,
		&evidence.HavePGTrgm, &evidence.KBTablesOK); err != nil {
		// A probe that cannot run is a store that is not ready, which is what
		// the evidence already says when every flag is clear.
		return db2contract.EncodeHealthResponse(db2contract.HealthEvidence{}),
			bus.ModuleStatusOK
	}
	return db2contract.EncodeHealthResponse(evidence), bus.ModuleStatusOK
}

// embeddingDimension answers the vector width this store is configured for.
//
// The C reads a process global set at startup; this reads the same value from
// the runtime configuration, falling back to what the schema recorded. Reading
// the schema is the better answer of the two: the recorded dimension is what
// the vector columns were actually created at, and a process configured
// differently from its own store is exactly the mismatch this is asked about.
const recordedEmbeddingDimensionQuery = `SELECT value
 FROM kb_meta WHERE key = 'schema_embedding_dim'`

func embeddingDimension(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEmbeddingDimensionRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	dimension := configuredEmbeddingDimension()
	if dimension == 0 {
		// Nothing configured: fall back to what the schema recorded, which is
		// the width the vector columns were actually created at. The C falls
		// back to a process default here; the recorded value is the better
		// answer, because it describes the store rather than the process.
		dimension = recordedEmbeddingDimension(ctx, store)
	}
	if dimension == 0 {
		// Neither configured nor recorded. Capability-absent rather than a
		// guess: a width guessed wrong writes vectors the store cannot hold.
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	reply, encodeErr := db2contract.EncodeEmbeddingDimensionReply(
		db2contract.ResultOK, uint32(dimension))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// poolStatus answers what this module's connection pool is doing.
//
// The lease counters the C reports come from its own hand-rolled pool, which
// tracks grants, timeouts, stuck leases and poisoned connections. pgxpool keeps
// a different set, so the fields that have no counterpart answer zero rather
// than being filled with something that looks like a measurement.
//
// Size, in-use and waiters do have counterparts, and those are real.
func poolStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodePoolStatusRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// An optional interface rather than a type assertion on *PoolStore: a
	// Store that wraps a pool -- the probe harness wraps one in a transaction
	// -- can still answer, and asserting the concrete type made this
	// capability-absent for every wrapper.
	provider, ok := store.(poolStatistics)
	if !ok {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	stat := provider.PoolStat()
	if stat == nil {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	status := db2contract.PoolStatus{
		Size:        uint32(max(int64(stat.TotalConns()), 0)),
		InUse:       uint32(max(int64(stat.AcquiredConns()), 0)),
		Waiters:     uint32(max(stat.EmptyAcquireCount(), 0)),
		LeaseGrants: uint64(max(stat.AcquireCount(), 0)),
		// pgxpool cancels rather than timing out, and has no notion of a stuck
		// or poisoned lease at all. Zero here means "not measured", and saying
		// so is better than inventing a number for a counter that does not
		// exist on this side.
		LeaseTimeouts: uint64(max(stat.CanceledAcquireCount(), 0)),
		Stuck:         0,
		Poisoned:      0,
	}
	reply, encodeErr := db2contract.EncodePoolStatusReply(db2contract.ResultOK,
		status)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// embeddingRefusals answers how often a vector of the wrong width was offered.
//
// The C counts these in the process that does the offering -- its pgvector
// transport increments an atomic when it rejects one. This module has no such
// transport yet, so it has counted nothing, and it says zero rather than
// reporting the C's count, which it cannot see.
//
// Zero from a module that has refused nothing and zero from a module that
// cannot count are the same reply, and that is a real limitation of the field
// rather than of this implementation: there is no "unknown" to answer.
func embeddingRefusals(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEmbeddingRefusalsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	reply, encodeErr := db2contract.EncodeEmbeddingRefusalsReply(
		db2contract.ResultOK, db2contract.EmbeddingRefusals{})
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// What the server itself can see about the database it is connected to.
//
// The lag is only meaningful on a standby, and the CASE keeps it that way: a
// primary answers zero rather than the negative or nonsensical value the WAL
// functions would produce there.
const postgresStatusQuery = `SELECT
 (SELECT COUNT(*) FROM pg_stat_activity WHERE datname = current_database()),
 current_setting('max_connections')::int,
 pg_is_in_recovery()::int,
 CASE WHEN pg_is_in_recovery()
   THEN COALESCE(pg_wal_lsn_diff(pg_last_wal_receive_lsn(),
     pg_last_wal_replay_lsn()), 0)::bigint
   ELSE 0 END`

func postgresStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodePostgresStatusRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var active, maximum, replica, lag int64
	if err := store.QueryRow(ctx, postgresStatusQuery).
		Scan(&active, &maximum, &replica, &lag); err != nil {
		// Unavailable rather than an error: the caller asked what the database
		// looks like and the answer is that it could not be asked. Every
		// availability bit stays clear, and the contract requires every value
		// to be zero when its bit is.
		reply, encodeErr := db2contract.EncodePostgresStatusReply(
			db2contract.ResultOK, db2contract.PostgresStatus{})
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	// Available is a bit set saying which of the four were actually read, not a
	// boolean saying the database answered, and a value must be zero wherever
	// its bit is clear -- which is what makes a zero legible as "not read"
	// rather than as "zero connections".
	//
	// The lag bit is the one that is conditional, and the contract enforces it:
	// a status carrying the lag bit is only valid when it is also a replica.
	// Lag on a primary is not a measurement, so on a primary the bit stays
	// clear and the value stays zero.
	available := db2contract.PostgresAvailableActive |
		db2contract.PostgresAvailableMax | db2contract.PostgresAvailableRole
	isReplica := uint32(max(replica, 0))
	lagBytes := uint64(0)
	if isReplica == 1 {
		available |= db2contract.PostgresAvailableLag
		lagBytes = uint64(max(lag, 0))
	}
	reply, encodeErr := db2contract.EncodePostgresStatusReply(
		db2contract.ResultOK, db2contract.PostgresStatus{
			Available:         available,
			ActiveConnections: uint32(max(active, 0)),
			MaxConnections:    uint32(max(maximum, 0)),
			IsReplica:         isReplica,
			ReplicaLagBytes:   lagBytes,
		})
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const (
	reembedStatusQuery = `SELECT value FROM kb_meta WHERE key = $1`

	reembedClearQuery = `DELETE FROM kb_meta WHERE key = $1`
)

// reembedStatus answers whether a re-embed is in progress, and at what width.
//
// The marker is one text value holding two numbers separated by a colon, which
// is the C's encoding and is parsed here the same way: a value that does not
// split cleanly still means "in progress", because the marker's presence is the
// claim and its contents are only detail.
func reembedStatus(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeReembedStatusRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var marker string
	switch err := store.QueryRow(ctx, reembedStatusQuery, reembedInProgressKey).
		Scan(&marker); {
	case errors.Is(err, pgx.ErrNoRows):
		marker = ""
	case err != nil:
		return nil, bus.ModuleStatusInternal
	}
	// An OK reply means a re-embed is in progress, and the contract enforces
	// it: the dimension must be in range and the epoch nonzero. "Not in
	// progress" is therefore not_found rather than an OK reply full of zeros,
	// which is the distinction the caller is asking about.
	result := db2contract.ResultNotFound
	status := db2contract.ReembedStatus{}
	if marker != "" {
		dimension, epoch := parseReembedMarker(marker)
		if dimension >= db2contract.ReembedDimensionMin &&
			dimension <= db2contract.ReembedDimensionMax && epoch > 0 {
			result = db2contract.ResultOK
			status.TargetDimension = dimension
			status.StartedEpoch = epoch
		}
		// A marker present but unreadable stays not_found. The C treats the
		// marker's presence as the claim, but its reply cannot carry a claim
		// without the two numbers, so there is nothing to answer with.
	}
	reply, encodeErr := db2contract.EncodeReembedStatusReply(result, status)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// parseReembedMarker reads "<dimension>:<epoch>", tolerating a missing epoch.
func parseReembedMarker(marker string) (uint32, uint64) {
	dimensionText, epochText, _ := strings.Cut(marker, ":")
	dimension, _ := strconv.ParseUint(strings.TrimSpace(dimensionText), 10, 32)
	epoch, _ := strconv.ParseUint(strings.TrimSpace(epochText), 10, 64)
	return uint32(dimension), epoch
}

func reembedClear(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeReembedClearRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, reembedClearQuery,
		reembedInProgressKey); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeReembedClearReply(db2contract.ResultOK)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The decision and the delete, under a lock, in one transaction.
//
// The lock is the C's and it is load-bearing: between reading the recorded
// dimension and deleting the marker, another writer must not change kb_meta, or
// the clear would be made on a store that has since moved. SHARE ROW EXCLUSIVE
// is what the C takes and what this takes.
const (
	reembedMaintenanceLockQuery = `LOCK TABLE kb_meta IN SHARE ROW EXCLUSIVE MODE`

	reembedMaintenanceReadQuery = `SELECT
 EXISTS (SELECT 1 FROM kb_meta WHERE key = $1),
 COALESCE((SELECT value FROM kb_meta WHERE key = 'schema_embedding_dim'), '')`
)

// reembedClearMaintenance clears the maintenance marker, refusing when the
// store is mid-transition unless forced.
//
// A recorded dimension that disagrees with the running one means the store is
// part-way through a change, and clearing the marker there would tell every
// reader the maintenance had finished when it had not. Force exists for the
// operator who knows better; without it this refuses.
func reembedClearMaintenance(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	force, err := db2contract.DecodeReembedClearMaintenanceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The reply must carry a running dimension in range -- the contract will
	// not encode a zero one -- so an unconfigured module falls back to what the
	// schema recorded, and refuses when neither is known. There is nothing
	// truthful to answer without it: the whole point of the reply is the
	// comparison between the recorded width and the running one.
	running := configuredEmbeddingDimension()
	if running == 0 {
		running = recordedEmbeddingDimension(ctx, store)
	}
	if running == 0 {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	status := db2contract.ReembedClearMaintenance{
		RunningDimension: uint32(running),
	}
	inconsistent := false
	txErr := store.InTx(ctx, func(tx Store) error {
		if _, lockErr := tx.Exec(ctx, reembedMaintenanceLockQuery); lockErr != nil {
			return lockErr
		}
		var inProgress bool
		var recordedText string
		if scanErr := tx.QueryRow(ctx, reembedMaintenanceReadQuery,
			reembedInProgressKey).Scan(&inProgress, &recordedText); scanErr != nil {
			return scanErr
		}
		if inProgress {
			status.WasInProgress = 1
		}
		recorded, _ := strconv.ParseUint(strings.TrimSpace(recordedText), 10, 32)
		status.RecordedDimension = uint32(recorded)
		if recorded > 0 && uint32(recorded) != uint32(running) && force == 0 {
			inconsistent = true
			return errReembedInconsistent
		}
		_, deleteErr := tx.Exec(ctx, reembedClearQuery, reembedInProgressKey)
		return deleteErr
	})
	if inconsistent {
		reply, encodeErr := db2contract.EncodeReembedClearMaintenanceReply(
			db2contract.ResultInvalidState, status)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	if txErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeReembedClearMaintenanceReply(
		db2contract.ResultOK, status)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

var errReembedInconsistent = errors.New(
	"db2: the recorded dimension disagrees with the running one")

// embedderServingID answers which embedder produced the vectors in this store.
//
// A process value in the C, and a configured one here. Empty is a real answer:
// a store whose vectors came from the builtin hash has no serving identity, and
// saying so is what lets a reader tell it from one that does.
func embedderServingID(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEmbedderServingIDRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	reply, encodeErr := db2contract.EncodeEmbedderServingIDReply(
		db2contract.ResultOK, configuredEmbedderServingID())
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The vector tables a dimension change would have to rebuild, and how much is
// in them.
//
// Discovered rather than listed, as the C discovers them: a table added later
// is one this has to find, and a hard-coded list would quietly skip it and
// leave vectors of the old width behind.
const dimensionResetPlanQuery = `SELECT
 c.relname,
 COALESCE((SELECT n_live_tup FROM pg_stat_user_tables s
   WHERE s.relname = c.relname), 0)
 FROM pg_attribute a
 JOIN pg_class c ON c.oid = a.attrelid
 JOIN pg_namespace n ON n.oid = c.relnamespace
 JOIN pg_type t ON t.oid = a.atttypid
 WHERE n.nspname = current_schema() AND c.relkind = 'r'
   AND t.typname = 'vector' AND a.attnum > 0 AND NOT a.attisdropped
 GROUP BY c.relname
 ORDER BY c.relname`

// dimensionReset plans a change of embedding width, and refuses to perform one.
//
// The plan is real: it discovers every vector table and counts what would be
// cleared, which is what a caller needs before deciding. Performing it is not
// done here, and the reason is specific rather than an omission.
//
// The C's execution drops those tables and then re-applies the entire schema at
// the new width, using its own schema applier -- fourteen thousand lines of SQL
// that the C module reaches through db_apply_schema_postgres and that this
// module has no access to. It then re-triggers three separate re-embedding
// pipelines and rewrites a process global. A Go implementation that dropped the
// tables without being able to recreate them would leave the store unusable,
// which is worse than refusing.
//
// So a dry run answers the plan, and an execution answers capability-absent:
// the honest reply for "this provider cannot do that". Moving it here means
// moving schema application here first, and that is the cutover's work.
func dimensionReset(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetDimension, force, dryRun, err :=
		db2contract.DecodeDimensionResetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_ = force
	if dryRun == 0 {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	status := db2contract.DimensionReset{TargetDimension: targetDimension}
	var recordedText string
	switch scanErr := store.QueryRow(ctx, recordedEmbeddingDimensionQuery).
		Scan(&recordedText); {
	case errors.Is(scanErr, pgx.ErrNoRows):
	case scanErr != nil:
		return nil, bus.ModuleStatusInternal
	}
	recorded, _ := strconv.ParseUint(strings.TrimSpace(recordedText), 10, 32)
	status.RecordedDimension = uint32(recorded)
	if uint32(recorded) == targetDimension {
		// Nothing to do, which the C reports as a successful no-op rather than
		// as a refusal.
		reply, encodeErr := db2contract.EncodeDimensionResetReply(
			db2contract.ResultOK, status)
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	rows, queryErr := store.Query(ctx, dimensionResetPlanQuery)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()
	for rows.Next() {
		var table string
		var live int64
		if scanErr := rows.Scan(&table, &live); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		status.TablesDiscovered++
		status.RowsCleared += uint64(max(live, 0))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeDimensionResetReply(
		db2contract.ResultOK, status)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// recordedEmbeddingDimension answers the width the schema recorded, or zero.
//
// Zero for a missing row, an unreadable value and a failed read alike: all
// three mean the store cannot say how wide its vectors are, and the callers
// treat that the same way.
func recordedEmbeddingDimension(ctx context.Context, store Store) int {
	var recorded *string
	if err := store.QueryRow(ctx, recordedEmbeddingDimensionQuery).
		Scan(&recorded); err != nil {
		return 0
	}
	parsed, err := strconv.ParseUint(strings.TrimSpace(text(recorded)), 10, 32)
	if err != nil {
		return 0
	}
	return int(parsed)
}

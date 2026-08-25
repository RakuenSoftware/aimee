package db2

import (
	"context"

	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The server-introspection statements, ported from db2_pg_stat_summary.
const (
	sqlActiveConnections = `SELECT count(*)::int FROM pg_stat_activity ` +
		`WHERE datname = current_database()`

	sqlMaxConnections = `SELECT current_setting('max_connections')::int`

	sqlIsReplica = `SELECT pg_is_in_recovery()::int`

	// Replication lag in WAL bytes. Meaningful only on a standby, and zero
	// rather than NULL when the standby is caught up.
	sqlReplicaLag = `SELECT COALESCE(` +
		`  pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn()), 0)::bigint`
)

// PoolStatsFunc reports the pool's own accounting.
//
// The pool is the only thing that knows its lease grants, timeouts, stuck
// holders and poisoned members, and none of those are derivable from a
// driver's connection statistics. Whoever owns the pool supplies this.
type PoolStatsFunc func(ctx context.Context) (db2contract.PoolStatus, error)

// LifecycleSeams is what the lifecycle provider needs from its host.
type LifecycleSeams struct {
	QueryRow  QueryRowFunc
	PoolStats PoolStatsFunc
}

type pgLifecycleBackend struct {
	queryRow  QueryRowFunc
	poolStats PoolStatsFunc
}

// NewPGLifecycleBackend builds the lifecycle family's production backend.
func NewPGLifecycleBackend(seams LifecycleSeams) LifecycleBackend {
	return &pgLifecycleBackend{queryRow: seams.QueryRow, poolStats: seams.PoolStats}
}

func (b *pgLifecycleBackend) HealthProbe(ctx context.Context) (db2contract.HealthEvidence, error) {
	evidence := db2contract.HealthEvidence{}
	if b == nil || b.queryRow == nil {
		return evidence, ErrNoQuerier
	}
	row := b.queryRow(ctx, healthQuery)
	if row == nil {
		return evidence, ErrNoQuerier
	}
	if err := row.Scan(&evidence.SchemaOK, &evidence.HavePGTrgm, &evidence.KBTablesOK); err != nil {
		return db2contract.HealthEvidence{}, err
	}
	return evidence, nil
}

// PostgresStatus reports each fact the server will answer for, and omits the
// ones it will not.
//
// Every field is optional and carries an availability bit. A sub-query that
// fails leaves its bit clear rather than failing the whole operation, which is
// what the C implementation does and what makes this useful against a server
// that withholds one view: `pg_stat_activity` is restricted for an unprivileged
// role, and losing the connection count that way must not also cost the caller
// the replica role it could have had.
func (b *pgLifecycleBackend) PostgresStatus(ctx context.Context) (db2contract.PostgresStatus, error) {
	if b == nil || b.queryRow == nil {
		return db2contract.PostgresStatus{}, ErrNoQuerier
	}
	status := db2contract.PostgresStatus{}

	if active, ok := b.optionalInt(ctx, sqlActiveConnections); ok && active >= 0 {
		status.Available |= db2contract.PostgresAvailableActive
		status.ActiveConnections = uint32(active)
	}
	if maximum, ok := b.optionalInt(ctx, sqlMaxConnections); ok && maximum >= 0 {
		status.Available |= db2contract.PostgresAvailableMax
		status.MaxConnections = uint32(maximum)
	}

	replica, replicaKnown := b.optionalInt(ctx, sqlIsReplica)
	if replicaKnown && (replica == 0 || replica == 1) {
		status.Available |= db2contract.PostgresAvailableRole
		status.IsReplica = uint32(replica)
	}

	// Lag is asked for only on a standby. A primary has no lag to report, and
	// reporting zero there would read as "caught up" rather than "not
	// applicable" — which is why the contract gates it behind the role bit.
	if replicaKnown && replica == 1 {
		if lag, ok := b.optionalInt(ctx, sqlReplicaLag); ok && lag >= 0 {
			status.Available |= db2contract.PostgresAvailableLag
			status.ReplicaLagBytes = uint64(lag)
		}
	}
	return status, nil
}

// optionalInt runs a single-value probe, reporting whether it answered.
//
// A failure is not propagated: the caller's contract is a partial answer, and
// distinguishing "the server refused this view" from "the database is down" is
// the health probe's job, not this one's.
func (b *pgLifecycleBackend) optionalInt(ctx context.Context, query string) (int64, bool) {
	row := b.queryRow(ctx, query)
	if row == nil {
		return 0, false
	}
	var value int64
	if err := row.Scan(&value); err != nil {
		return 0, false
	}
	return value, true
}

func (b *pgLifecycleBackend) PoolStatus(ctx context.Context) (db2contract.PoolStatus, error) {
	if b == nil || b.poolStats == nil {
		return db2contract.PoolStatus{}, ErrNoQuerier
	}
	return b.poolStats(ctx)
}

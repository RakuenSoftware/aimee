package db2

import (
	"context"
	"errors"
	"fmt"
	"time"

	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The memory family's SQL, ported from the C module so the two implementations
// answer identically for the same database.
//
// The statements are kept verbatim rather than "improved" during the port. A
// migration that also rewrites its queries cannot tell a porting defect from a
// deliberate change when the two sides disagree, and these have to be
// differentially testable against the C module until the cutover.
//
// Placeholders are libpq's $N here where the C source writes ?N: the C layer
// rewrites its own dialect before it reaches the server, so $N is the same
// statement, not a different one.
const (
	sqlLevel3Count     = `SELECT COUNT(*) FROM memories WHERE tier = 'L3'`
	sqlLevel2Count     = `SELECT COUNT(*) FROM memories WHERE tier = 'L2'`
	sqlTotalCount      = `SELECT COUNT(*) FROM memories`
	sqlSessionL2Count  = `SELECT COUNT(*) FROM memories WHERE tier = 'L2' AND source_session = $1`
	sqlKeyExists       = `SELECT 1 FROM memories WHERE key = $1 LIMIT 1`
	sqlFindIDByKeyKind = `SELECT id FROM memories WHERE key = $1 AND kind = $2 LIMIT 1`
	sqlKeyExistsInTier = `SELECT 1 FROM memories WHERE key = $1 AND tier IN ($2, $3) LIMIT 1`

	// pg_now_text is the module's own clock function. The horizon stays inside
	// the statement, evaluated by the database, so the answer does not depend on
	// the caller's clock agreeing with the server's.
	sqlOrphanedL0Count = `SELECT COUNT(*) FROM memories WHERE tier = 'L0'` +
		` AND created_at < pg_now_text('-7 days')`
)

// ErrNoQuerier reports a backend built without a database seam. It is a
// programming error rather than a runtime condition, and it is distinct from a
// query failure so the handler does not report a missing pool as a sick
// database.
var ErrNoQuerier = errors.New("db2: memory backend has no querier")

// Rows is the multi-row half of the database seam, kept to the three calls a
// caller actually needs so a test can satisfy it without a driver.
type Rows interface {
	Next() bool
	Scan(dest ...any) error
	Err() error
	Close()
}

// QueryFunc runs a statement returning many rows.
type QueryFunc func(ctx context.Context, sql string, args ...any) (Rows, error)

// ExecFunc runs a statement that returns no rows, answering how many it
// affected. The count is the operation's whole answer for the sweeps, so it is
// returned rather than discarded.
type ExecFunc func(ctx context.Context, sql string, args ...any) (int64, error)

// MemorySeams is the set of database capabilities the memory family needs.
//
// They are separate fields rather than one fat interface because the families
// need different subsets, and a provider that can only read should not have to
// supply a writer it will never be asked for.
type MemorySeams struct {
	QueryRow QueryRowFunc
	Query    QueryFunc
	Exec     ExecFunc
	// Now stamps rows the C implementation stamps client-side. Injected so a
	// test can assert the written timestamp rather than tolerate any value.
	Now func() time.Time
}

// pgMemoryBackend implements MemoryBackend over the seams above. It holds no
// pool and knows no DSN: the process that owns the pool supplies the seams,
// which is what keeps the credential out of the bus handler.
type pgMemoryBackend struct {
	queryRow QueryRowFunc
	query    QueryFunc
	exec     ExecFunc
	clock    func() time.Time
}

// NewPGMemoryBackend builds the memory family's production backend.
func NewPGMemoryBackend(seams MemorySeams) MemoryBackend {
	return &pgMemoryBackend{
		queryRow: seams.QueryRow,
		query:    seams.Query,
		exec:     seams.Exec,
		clock:    seams.Now,
	}
}

func (b *pgMemoryBackend) now() time.Time {
	if b == nil || b.clock == nil {
		return time.Now()
	}
	return b.clock()
}

// execCounted runs a mutation and, when affected is non-nil, range-checks the
// affected-row count into it.
func (b *pgMemoryBackend) execCounted(ctx context.Context, query string, affected *uint32, args ...any) error {
	if b == nil || b.exec == nil {
		return ErrNoQuerier
	}
	n, err := b.exec(ctx, query, args...)
	if err != nil {
		return err
	}
	if n < 0 {
		return fmt.Errorf("db2: negative affected-row count %d from %q", n, query)
	}
	if affected != nil {
		*affected = uint32(n)
	}
	return nil
}

// scanOne runs a single-row query and scans it into dest.
func (b *pgMemoryBackend) scanOne(ctx context.Context, query string, dest []any, args ...any) error {
	if b == nil || b.queryRow == nil {
		return ErrNoQuerier
	}
	row := b.queryRow(ctx, query, args...)
	if row == nil {
		return fmt.Errorf("db2: querier returned no row for %q", query)
	}
	return row.Scan(dest...)
}

// count runs a COUNT(*) and range-checks it into a uint32.
//
// The scan target is int64 because that is what the server sends for COUNT(*),
// and the conversion is checked rather than assumed: a negative value would
// wrap to an enormous count, which is exactly the kind of wrong-but-plausible
// answer this layer must not produce.
func (b *pgMemoryBackend) count(ctx context.Context, query string, args ...any) (uint32, error) {
	var n int64
	if err := b.scanOne(ctx, query, []any{&n}, args...); err != nil {
		return 0, err
	}
	if n < 0 {
		return 0, fmt.Errorf("db2: negative count %d from %q", n, query)
	}
	return uint32(n), nil
}

// exists answers a `SELECT 1 … LIMIT 1` probe.
//
// No rows is a legitimate answer meaning "absent", not a failure, so ErrNoRows
// is folded into false here. Every caller of this shape in the C module treats
// the two the same way; making the Go side surface it as an error would turn a
// routine miss into a reported database fault.
func (b *pgMemoryBackend) exists(ctx context.Context, query string, args ...any) (bool, error) {
	var probe int
	err := b.scanOne(ctx, query, []any{&probe}, args...)
	if err != nil {
		if isNoRows(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

func (b *pgMemoryBackend) Level3Count(ctx context.Context) (uint32, error) {
	return b.count(ctx, sqlLevel3Count)
}

func (b *pgMemoryBackend) Level2Count(ctx context.Context) (uint32, error) {
	return b.count(ctx, sqlLevel2Count)
}

func (b *pgMemoryBackend) OrphanedL0Count(ctx context.Context) (uint32, error) {
	return b.count(ctx, sqlOrphanedL0Count)
}

func (b *pgMemoryBackend) TotalCount(ctx context.Context) (uint64, error) {
	var n int64
	if err := b.scanOne(ctx, sqlTotalCount, []any{&n}); err != nil {
		return 0, err
	}
	if n < 0 {
		return 0, fmt.Errorf("db2: negative count %d from %q", n, sqlTotalCount)
	}
	return uint64(n), nil
}

// SessionL2Count refuses an empty session rather than counting every L2 memory.
//
// The C implementation returns 0 for an empty session before it reaches the
// database. Preserving that here matters: `source_session = ”` is a legal
// query that would answer with a real count for a caller that meant to name a
// session and did not.
func (b *pgMemoryBackend) SessionL2Count(ctx context.Context, sourceSession string) (uint32, error) {
	if sourceSession == "" {
		return 0, nil
	}
	return b.count(ctx, sqlSessionL2Count, sourceSession)
}

func (b *pgMemoryBackend) KeyExists(ctx context.Context, key string) (bool, error) {
	if key == "" {
		return false, nil
	}
	return b.exists(ctx, sqlKeyExists, key)
}

func (b *pgMemoryBackend) FindIDByKeyKind(ctx context.Context, key, kind string) (bool, uint64, error) {
	if key == "" || kind == "" {
		return false, 0, nil
	}
	var id int64
	err := b.scanOne(ctx, sqlFindIDByKeyKind, []any{&id}, key, kind)
	if err != nil {
		if isNoRows(err) {
			return false, 0, nil
		}
		return false, 0, err
	}
	if id < 0 {
		return false, 0, fmt.Errorf("db2: negative memory id %d", id)
	}
	return true, uint64(id), nil
}

func (b *pgMemoryBackend) KeyExistsInTierPair(ctx context.Context, key, tierA, tierB string) (bool, error) {
	if key == "" || tierA == "" || tierB == "" {
		return false, nil
	}
	return b.exists(ctx, sqlKeyExistsInTier, key, tierA, tierB)
}

// The memory family's mutations and aggregates, ported from memory_health.c.
const (
	sqlUpdateEffectiveness = `UPDATE memories SET effectiveness = $1 WHERE id = $2`

	sqlRetentionDelete = `DELETE FROM memories` +
		` WHERE sensitivity = $1` +
		`   AND created_at < pg_now_text($2)`

	sqlDemoteLowEffectiveness = `UPDATE memories SET tier = 'L1', updated_at = $1` +
		` WHERE tier = 'L2'` +
		`   AND effectiveness IS NOT NULL` +
		`   AND effectiveness < $2`

	sqlEffectivenessStats = `SELECT` +
		` AVG(CASE WHEN effectiveness IS NOT NULL THEN effectiveness END),` +
		` SUM(CASE WHEN effectiveness IS NOT NULL` +
		`             AND effectiveness < $1 THEN 1 ELSE 0 END),` +
		` SUM(CASE WHEN effectiveness IS NOT NULL` +
		`             AND effectiveness > 0.8 AND use_count >= 10` +
		`          THEN 1 ELSE 0 END)` +
		` FROM memories`

	sqlListL2MemoryIDs = `SELECT id FROM memories WHERE tier = 'L2' LIMIT $1`
)

// nowUTC formats an instant the way db2_now_utc does.
//
// The timestamp is generated by the caller rather than the server because the
// C implementation does the same and the column is compared against values
// written that way. Changing which clock stamps the row is a data change
// disguised as a port.
func nowUTC(now time.Time) string {
	return now.UTC().Format("2006-01-02T15:04:05Z")
}

func (b *pgMemoryBackend) SetEffectiveness(ctx context.Context, memoryID uint64, value float64) error {
	return b.updateEffectiveness(ctx, memoryID, value)
}

// ClearEffectiveness writes SQL NULL, not zero. Zero is a measured score of
// nothing; NULL is the absence of a measurement, and the demotion sweep
// selects on `effectiveness IS NOT NULL` precisely to tell them apart.
func (b *pgMemoryBackend) ClearEffectiveness(ctx context.Context, memoryID uint64) error {
	return b.updateEffectiveness(ctx, memoryID, nil)
}

func (b *pgMemoryBackend) updateEffectiveness(ctx context.Context, memoryID uint64, value any) error {
	if memoryID == 0 {
		return fmt.Errorf("db2: effectiveness update needs a memory id")
	}
	return b.execCounted(ctx, sqlUpdateEffectiveness, nil, value, int64(memoryID))
}

func (b *pgMemoryBackend) RetentionDelete(ctx context.Context, sensitivity string, days uint32) (uint32, error) {
	if sensitivity == "" || days == 0 {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlRetentionDelete, &affected,
		sensitivity, fmt.Sprintf("-%d days", days))
	return affected, err
}

func (b *pgMemoryBackend) DemoteLowEffectiveness(ctx context.Context, threshold float64) (uint32, error) {
	var affected uint32
	err := b.execCounted(ctx, sqlDemoteLowEffectiveness, &affected,
		nowUTC(b.now()), threshold)
	return affected, err
}

func (b *pgMemoryBackend) EffectivenessStats(ctx context.Context, lowThreshold float64) (db2contract.EffectivenessStats, error) {
	// Every column is nullable: AVG over no rows is NULL, and so is SUM. An
	// empty table is a legitimate state, so the nulls become zeroes here rather
	// than failing the operation.
	var (
		average   *float64
		low       *int64
		impactful *int64
	)
	if err := b.scanOne(ctx, sqlEffectivenessStats,
		[]any{&average, &low, &impactful}, lowThreshold); err != nil {
		if isNoRows(err) {
			return db2contract.EffectivenessStats{}, nil
		}
		return db2contract.EffectivenessStats{}, err
	}
	stats := db2contract.EffectivenessStats{}
	if average != nil {
		stats.AvgEffectiveness = *average
	}
	if low != nil && *low > 0 {
		stats.LowEffectivenessCount = uint32(*low)
	}
	if impactful != nil && *impactful > 0 {
		stats.HighImpactCount = uint32(*impactful)
	}
	return stats, nil
}

func (b *pgMemoryBackend) ListL2MemoryIDs(ctx context.Context, max uint32) ([]uint64, error) {
	if b == nil || b.query == nil {
		return nil, ErrNoQuerier
	}
	if max == 0 {
		return nil, nil
	}
	rows, err := b.query(ctx, sqlListL2MemoryIDs, int64(max))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	ids := make([]uint64, 0, 16)
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		// The contract's floor is 1. A row that cannot be represented is
		// dropped rather than encoded as something else, and the reply's own
		// bound catches a list that grew past what the wire allows.
		if id < int64(db2contract.L2MemoryIDMin) {
			continue
		}
		ids = append(ids, uint64(id))
		if uint32(len(ids)) >= max {
			break
		}
	}
	return ids, rows.Err()
}

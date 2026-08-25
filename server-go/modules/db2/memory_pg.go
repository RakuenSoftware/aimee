package db2

import (
	"context"
	"errors"
	"fmt"
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

// pgMemoryBackend implements MemoryBackend over the package's QueryRowFunc
// seam. It holds no pool and knows no DSN: the process that owns the pool
// supplies the seam, which is what keeps the credential out of the bus handler.
type pgMemoryBackend struct {
	queryRow QueryRowFunc
}

// NewPGMemoryBackend builds the memory family's production backend from a
// query seam.
func NewPGMemoryBackend(queryRow QueryRowFunc) MemoryBackend {
	return &pgMemoryBackend{queryRow: queryRow}
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

package families

import (
	"context"
	"errors"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
)

type cacheReplayQueryer struct{ traceReplayQueryer }

func (q cacheReplayQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return cacheReplayRow{q.Tx.QueryRow(ctx, sql, args...)}
}

type cacheReplayRow struct{ pgx.Row }

func (r cacheReplayRow) Scan(dest ...any) error {
	err := r.Row.Scan(dest...)
	if errors.Is(err, pgx.ErrNoRows) {
		return store.ErrNoRows
	}
	return err
}

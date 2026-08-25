package db2

import (
	"database/sql"
	"errors"

	"github.com/jackc/pgx/v5"
)

// isNoRows reports the "query matched nothing" sentinel.
//
// Both spellings are matched because the seam is an interface, not a driver:
// a pgx-backed querier answers pgx.ErrNoRows, and a database/sql-backed or test
// querier answers sql.ErrNoRows. Recognising only one would make an ordinary
// miss look like a database fault depending on who supplied the seam.
func isNoRows(err error) bool {
	return errors.Is(err, pgx.ErrNoRows) || errors.Is(err, sql.ErrNoRows)
}

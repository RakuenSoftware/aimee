package db2

import (
	"database/sql"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
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

// statusForError maps a backend failure onto the bus status the caller should
// see.
//
// A missing seam means this module was built without that capability, which is
// a different answer from a database that failed: the first tells a caller to
// stop asking, the second tells it to retry. Collapsing both into an internal
// error made an unconfigured module look like a sick one.
func statusForError(err error) bus.ModuleStatus {
	if errors.Is(err, ErrNoQuerier) {
		return bus.ModuleStatusCapabilityAbsent
	}
	return bus.ModuleStatusInternal
}

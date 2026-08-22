package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
)

// Many catalogued reads return one column and nothing else. These two collect
// such a column, so an operation is left holding only what is specific to it:
// its statement, its ceiling, and the row type it fills.
//
// The ceiling is applied while scanning rather than as a LIMIT, which is what
// the C callers do. The distinction matters for a statement that has no
// ORDER BY, where adding a LIMIT would let the planner return a different set
// rather than the same set cut short.

func readTextColumn(ctx context.Context, store Store, ceiling int, query string, args ...any) (
	[]string, bus.ModuleStatus,
) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]string, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		// A pointer target because a NULL column is not a failure to any of
		// these reads: it is an absent value, and text() spells that empty.
		var value *string
		if err := rows.Scan(&value); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, text(value))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

func readIntColumn(ctx context.Context, store Store, ceiling int, query string, args ...any) (
	[]int64, bus.ModuleStatus,
) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	found := make([]int64, 0, ceiling)
	for rows.Next() {
		if len(found) == ceiling {
			break
		}
		var value *int64
		if err := rows.Scan(&value); err != nil {
			return nil, bus.ModuleStatusInternal
		}
		found = append(found, number(value))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	return found, bus.ModuleStatusOK
}

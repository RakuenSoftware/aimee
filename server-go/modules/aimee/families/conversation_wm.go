package families

import (
	"context"
	"fmt"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Working memory: per-session key/value entries with an optional expiry.

// wmMaxResults bounds a list or a search. The C carried a fixed array of this
// size and stopped filling it; here it is a LIMIT, which means the database
// stops producing rows rather than the module stopping reading them.
const wmMaxResults = 200

// Timestamps are compared in the database rather than against a string the
// module formats. The C built an ISO string in C and compared it as TEXT, so
// the comparison was lexicographic and depended on every writer having used the
// same format -- and on the module's clock, not the store's.
const wmColumns = `id, session_id, key, value, category,
                   to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                   to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                   COALESCE(to_char(expires_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'), '')`

const (
	// wmSetSQL upserts an entry, preserving created_at.
	//
	// The C used INSERT OR REPLACE, which DELETES the existing row and inserts
	// a new one -- so created_at was rewritten on every write and stopped
	// meaning "created". Any caller reasoning about how long something had been
	// held was reading the time of the last touch. ON CONFLICT updates in
	// place, so created_at is the creation and updated_at is the touch.
	//
	// A ttl of 0 or less means no expiry, which is what a NULL expires_at is.
	wmSetSQL = `INSERT INTO working_memory
	                (session_id, key, value, category, expires_at)
	            VALUES ($1, $2, $3, $4,
	                    CASE WHEN $5::bigint > 0
	                         THEN now() + make_interval(secs => $5::bigint)
	                         ELSE NULL END)
	            ON CONFLICT (session_id, key) DO UPDATE
	                SET value      = EXCLUDED.value,
	                    category   = EXCLUDED.category,
	                    expires_at = EXCLUDED.expires_at,
	                    updated_at = now()`

	wmGetSQL = `SELECT ` + wmColumns + `
	              FROM working_memory
	             WHERE session_id = $1 AND key = $2
	               AND (expires_at IS NULL OR expires_at > now())`

	// One statement rather than the C's two, chosen at runtime by whether a
	// category was given. An empty category means "every category", which is
	// the same question the C asked with its second statement.
	wmListSQL = `SELECT ` + wmColumns + `
	               FROM working_memory
	              WHERE session_id = $1
	                AND (expires_at IS NULL OR expires_at > now())
	                AND ($2 = '' OR category = $2)
	              ORDER BY updated_at DESC, id DESC
	              LIMIT $3`

	// The pattern is built here from a RAW query, escaped, and matched
	// case-insensitively. SQLite's LIKE is case-insensitive for ASCII by
	// default and PostgreSQL's is not, so a faithful translation is ILIKE;
	// leaving it as LIKE would quietly stop finding anything typed in a
	// different case.
	wmSearchSQL = `SELECT DISTINCT session_id
	                 FROM working_memory
	                WHERE key ILIKE $1 ESCAPE '\' OR value ILIKE $1 ESCAPE '\'
	                ORDER BY session_id DESC
	                LIMIT $2`

	wmDeleteSQL = `DELETE FROM working_memory WHERE session_id = $1 AND key = $2`
	wmClearSQL  = `DELETE FROM working_memory WHERE session_id = $1`

	// One statement, and the count is what was actually deleted.
	//
	// The C counted with one statement and deleted with another, so the number
	// it reported was the number of rows that had expired when it looked, not
	// the number it removed. A concurrent writer between the two made the
	// report wrong in both directions.
	wmGCSQL = `DELETE FROM working_memory
	            WHERE expires_at IS NOT NULL AND expires_at <= now()`
)

// wmRow reads one entry as its eight wire cells.
func wmRow(scan func(...any) error) ([]string, error) {
	var (
		id                              int64
		sessionID, key, value, category string
		createdAt, updatedAt, expiresAt string
	)
	if err := scan(&id, &sessionID, &key, &value, &category,
		&createdAt, &updatedAt, &expiresAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), sessionID, key, value, category,
		createdAt, updatedAt, expiresAt,
	}, nil
}

func wmSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sessionID, key := f[0], f[1]
	if sessionID == "" || key == "" {
		return store.StatusInvalid, nil, nil
	}
	ttl, ok := store.Atoi64(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	category := f[3]
	if category == "" {
		category = "general"
	}
	if _, err := q.Exec(ctx, wmSetSQL, sessionID, key, f[2], category, ttl); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func wmGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := wmRow(func(dest ...any) error {
		return q.QueryRow(ctx, wmGetSQL, f[0], f[1]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func wmList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, wmListSQL, 8, wmRow, f[0], f[1], max)
}

// wmSearchSessionIDs finds sessions whose working memory mentions the query.
//
// f[0] is the caller's raw text, not a pattern. The C wrapped it in %...% by
// string concatenation with no escaping, so a query containing % or _ matched
// far more than it asked for -- the same defect this migration found in the git
// and session families.
func wmSearchSessionIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, wmSearchSQL, 1, func(scan func(...any) error) ([]string, error) {
		var sessionID string
		if err := scan(&sessionID); err != nil {
			return nil, err
		}
		return []string{sessionID}, nil
	}, likeContains(f[0]), max)
}

func wmDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, wmDeleteSQL, f[0], f[1])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

// wmClear empties a session. Clearing a session that held nothing is not a
// miss: the caller asked for it to be empty and it is.
func wmClear(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, wmClearSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// wmGC removes expired entries and reports how many it removed.
func wmGC(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	tag, err := q.Exec(ctx, wmGCSQL)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// wmAssembleContext renders a session's working memory as the markdown block
// the caller splices into a prompt.
//
// A session with nothing in it produces no block at all rather than a heading
// with nothing under it, which is what the C's NULL return meant.
func wmAssembleContext(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, wmListSQL, f[0], "", wmMaxResults)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	defer rows.Close()

	var b strings.Builder
	entries := 0
	for rows.Next() {
		cells, err := wmRow(rows.Scan)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		if entries == 0 {
			b.WriteString("## Working Memory\n")
		}
		// cells are id, session_id, key, value, category, ...
		fmt.Fprintf(&b, "[%s] %s: %s\n", cells[4], cells[2], cells[3])
		entries++
	}
	if err := rows.Err(); err != nil {
		return store.StatusFailed, nil, err
	}
	if entries == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{b.String()}, nil
}

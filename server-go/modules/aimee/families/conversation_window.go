package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Context windows: a session's conversation split into numbered windows, each
// with a summary, a tier, and the terms and files it mentions.

// windowMaxTerms is how many terms a search may carry. The C checked this on
// the wire (count <= 1 + 32) and built one placeholder per term; here the terms
// travel as a single array parameter, so the bound is about what a caller may
// reasonably ask rather than about how long a statement can get.
const windowMaxTerms = 32

const (
	windowScanStateSQL = `SELECT COUNT(*), COALESCE(MAX(seq), 0)
	                        FROM windows WHERE session_id = $1`

	windowSessionIDSQL = `SELECT session_id FROM windows WHERE id = $1`

	windowCreateRawSQL = `INSERT INTO windows (session_id, seq, summary, created_at, tier)
	                      VALUES ($1, $2, $3, $4::timestamptz, 'raw') RETURNING id`

	windowAddTermSQL = `INSERT INTO window_terms (window_id, term) VALUES ($1, $2)`
	windowAddFileSQL = `INSERT INTO window_files (window_id, file_path) VALUES ($1, $2)`

	// The age is computed by the store. The C interpolated the day count into a
	// datetime() modifier string, so the interval was assembled by concatenation
	// and the comparison ran against the module's idea of "now" rather than the
	// store's.
	windowIDsByTierSQL = `SELECT id FROM windows
	                       WHERE tier = $1
	                         AND created_at < now() - make_interval(days => $2::int)
	                       ORDER BY id LIMIT $3`

	// Terms arrive as one array parameter rather than a statement built with
	// one placeholder per term.
	//
	// The C lowered the stored term but bound the caller's term RAW, so the
	// comparison was asymmetric: LOWER(wt.term) could never equal a term with
	// any capital in it, and a caller searching for "Deploy" matched nothing at
	// all. Both sides are lowered here.
	windowCandidatesByTermsSQL = `SELECT w.id, w.session_id, w.seq, w.summary,
	                                     to_char(w.created_at AT TIME ZONE 'utc',
	                                             'YYYY-MM-DD HH24:MI:SS'),
	                                     COUNT(DISTINCT lower(wt.term)) AS match_count
	                                FROM windows w
	                                JOIN window_terms wt ON wt.window_id = w.id
	                               WHERE lower(wt.term) = ANY (
	                                         SELECT lower(t) FROM unnest($1::text[]) AS t)
	                               GROUP BY w.id
	                               ORDER BY match_count DESC, w.id
	                               LIMIT $2`

	windowListFilesSQL = `SELECT file_path FROM window_files
	                       WHERE window_id = $1 ORDER BY id LIMIT $2`

	// The full-text search.
	//
	// SQLite matched a hand-built FTS5 query -- the terms quoted and joined with
	// OR into one string -- against a separate virtual table. Here each term is
	// its own parameterised tsquery and the index is derived from the summary
	// itself, so there is no query string to assemble and nothing to keep in
	// step with the row.
	//
	// Two things about the result differ deliberately:
	//
	//   The C's statement had no ORDER BY and no LIMIT. It stopped reading after
	//   `max` rows, so it returned the first `max` hits in ROWID order -- an
	//   arbitrary subset, not the best matches. Ranking before limiting is what
	//   makes "the top N hits" true.
	//
	//   ts_rank is positive-better and FTS5's bm25 rank is negative-better. The
	//   rank is negated so the sign convention the caller already sorts by is
	//   unchanged; a caller ordering ascending still gets its best match first.
	windowLexicalHitsSQL = `SELECT w.id,
	                               -MAX(ts_rank(to_tsvector('english', w.summary),
	                                            plainto_tsquery('english', t))) AS rank
	                          FROM windows w
	                          JOIN unnest($1::text[]) AS t
	                            ON to_tsvector('english', w.summary)
	                               @@ plainto_tsquery('english', t)
	                         GROUP BY w.id
	                         ORDER BY rank ASC, w.id
	                         LIMIT $2`

	windowExistsSQL  = `SELECT 1 FROM windows WHERE id = $1`
	windowSetTierSQL = `UPDATE windows SET tier = $2 WHERE id = $1`

	// The prunes keep the top N and delete the rest.
	//
	// The C selected SQLite's implicit rowid to decide what to keep. There is no
	// rowid here, so both tables carry an explicit identity column and the
	// ordering the prune depends on is a declared column rather than an artefact
	// of the storage engine.
	windowPruneTermsSQL = `DELETE FROM window_terms
	                        WHERE window_id = $1
	                          AND id NOT IN (SELECT id FROM window_terms
	                                          WHERE window_id = $1
	                                          ORDER BY LENGTH(term) DESC, term
	                                          LIMIT $2)`

	windowDeleteAllFilesSQL = `DELETE FROM window_files WHERE window_id = $1`

	windowPruneFilesSQL = `DELETE FROM window_files
	                        WHERE window_id = $1
	                          AND id NOT IN (SELECT id FROM window_files
	                                          WHERE window_id = $1
	                                          ORDER BY id
	                                          LIMIT $2)`

	windowsDeleteAfterTurnSQL = `DELETE FROM windows WHERE session_id = $1 AND seq > $2`
)

func windowScanState(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var count, maxSeq int64
	if err := q.QueryRow(ctx, windowScanStateSQL, f[0]).Scan(&count, &maxSeq); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(count), store.I64toa(maxSeq)}, nil
}

func windowSessionID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var sessionID string
	err := q.QueryRow(ctx, windowSessionIDSQL, id).Scan(&sessionID)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{sessionID}, nil
}

func windowCreateRaw(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	seq, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, windowCreateRawSQL, f[0], seq, f[2], f[3]).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

// addWindowChild is the shape both the term and file inserts have.
func addWindowChild(ctx context.Context, q store.Queryer, sql string, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, sql, id, f[1]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func windowAddTerm(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return addWindowChild(ctx, q, windowAddTermSQL, f)
}

func windowAddFile(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return addWindowChild(ctx, q, windowAddFileSQL, f)
}

func windowIDsByTier(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	days, ok := store.Atoi(f[1])
	if !ok || days < 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, windowIDsByTierSQL, 1, func(scan func(...any) error) ([]string, error) {
		var id int64
		if err := scan(&id); err != nil {
			return nil, err
		}
		return []string{store.I64toa(id)}, nil
	}, f[0], days, max)
}

// windowTerms reads a variadic term list: field 0 is the row limit and the rest
// are the terms.
//
// A term list that is empty, or longer than the wire allows, is refused rather
// than truncated. Truncating a search silently answers a narrower question than
// the caller asked.
func windowTerms(f []string) (terms []string, max int, ok bool) {
	// The bound belongs HERE, not only in the callers.
	//
	// This indexes f[0] and slices f[1:], and both callers happen to check
	// len(f) < 2 first -- so it is correct by convention, in two places, while
	// the indexing lives in a third. A third caller added without the check
	// panics on a frame the wire is free to deliver: these operations are
	// Args -1, so dispatch does not check their width and a 0- or 1-cell frame
	// reaches the handler.
	//
	// Cheaper to make it impossible than to remember. The callers' checks stay:
	// they answer StatusInvalid with the operation's own name in the log, which
	// is more useful than this returning false, and a redundant guard has never
	// been the problem.
	if len(f) < 2 {
		return nil, 0, false
	}
	max, ok = boundedMax(f[0])
	if !ok {
		return nil, 0, false
	}
	for _, term := range f[1:] {
		if term != "" {
			terms = append(terms, term)
		}
	}
	if len(terms) == 0 || len(terms) > windowMaxTerms {
		return nil, 0, false
	}
	return terms, max, true
}

func windowCandidatesByTerms(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) < 2 {
		return store.StatusInvalid, nil, nil
	}
	terms, max, ok := windowTerms(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, windowCandidatesByTermsSQL, 6, func(scan func(...any) error) ([]string, error) {
		var (
			id, seq, matchCount       int64
			sessionID, summary, stamp string
		)
		if err := scan(&id, &sessionID, &seq, &summary, &stamp, &matchCount); err != nil {
			return nil, err
		}
		return []string{
			store.I64toa(id), sessionID, store.I64toa(seq),
			summary, stamp, store.I64toa(matchCount),
		}, nil
	}, terms, max)
}

func windowListFiles(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, windowListFilesSQL, 1, func(scan func(...any) error) ([]string, error) {
		var path string
		if err := scan(&path); err != nil {
			return nil, err
		}
		return []string{path}, nil
	}, id, max)
}

// windowIndexSummary has no index to write.
//
// The C kept a second copy of the summary in an FTS5 virtual table and this
// operation was the call that put it there -- so the search index was only ever
// as current as the caller's discipline about calling it, and a summary written
// without this call stayed searchable under its old text.
//
// The PostgreSQL index is derived from windows.summary, so it cannot disagree
// with the row it indexes and there is nothing for a caller to keep in step.
// The operation stays on the wire, and stays honest about the window: indexing
// one that does not exist is a miss, not a silent success.
func windowIndexSummary(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var one int
	err := q.QueryRow(ctx, windowExistsSQL, id).Scan(&one)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func windowLexicalHits(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) < 2 {
		return store.StatusInvalid, nil, nil
	}
	terms, max, ok := windowTerms(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, windowLexicalHitsSQL, 2, func(scan func(...any) error) ([]string, error) {
		var (
			id   int64
			rank float64
		)
		if err := scan(&id, &rank); err != nil {
			return nil, err
		}
		return []string{store.I64toa(id), store.Ftoa(rank)}, nil
	}, terms, max)
}

func windowSetTier(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, windowSetTierSQL, id, f[1])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

// prune keeps the top `keep` rows for a window and deletes the rest.
func prune(ctx context.Context, q store.Queryer, sql string, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	keep, ok := store.Atoi64(f[1])
	if !ok || keep <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, sql, id, keep)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func windowPruneTerms(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return prune(ctx, q, windowPruneTermsSQL, f)
}

func windowPruneFiles(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return prune(ctx, q, windowPruneFilesSQL, f)
}

func windowDeleteAllFiles(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, windowDeleteAllFilesSQL, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// windowsDeleteAfterTurn drops every window past a sequence number, which is
// how a session is rewound.
func windowsDeleteAfterTurn(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	seq, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, windowsDeleteAfterTurnSQL, f[0], seq)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

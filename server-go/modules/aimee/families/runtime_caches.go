package families

import (
	"context"
	"strings"
	"unicode/utf8"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Caches, context snapshots, file snapshots, decisions, and the OSV advisory
// cache.

// Reply widths, from the catalog.
const (
	fsnapCells = 6
	osvCells   = 8
)

// --- the context and agent caches --------------------------------------------------

const (
	contextCacheGetSQL = `SELECT output FROM context_cache WHERE hash = $1`

	// The hash IS the key, so re-putting it stores the same output; what moves
	// is created_at, which is this entry's freshness.
	contextCachePutSQL = `INSERT INTO context_cache (hash, output, created_at)
	                      VALUES ($1, $2, now())
	                      ON CONFLICT (hash) DO UPDATE SET
	                          output = EXCLUDED.output, created_at = now()`

	contextCacheInvalidateSQL = `DELETE FROM context_cache`

	agentCacheGetSQL = `SELECT result FROM agent_cache WHERE role = $1 AND prompt = $2`

	// The role and prompt together are the key.
	//
	// The C's table had a surrogate id and no uniqueness at all, so its INSERT
	// OR REPLACE never replaced anything: every put appended a row, the table
	// grew without bound, and a get returned whichever duplicate the scan
	// reached first -- which could be an older answer than the one just stored.
	agentCachePutSQL = `INSERT INTO agent_cache (role, prompt, result, created_at)
	                    VALUES ($1, $2, $3, now())
	                    ON CONFLICT (role, prompt) DO UPDATE SET
	                        result = EXCLUDED.result, created_at = now()`
)

func contextCacheGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var output string
	err := q.QueryRow(ctx, contextCacheGetSQL, f[0]).Scan(&output)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{output, "0"}, nil
}

func contextCachePut(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, contextCachePutSQL, f[0], f[1]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// contextCacheInvalidate empties the cache. Emptying an empty cache is success:
// the caller asked for it to be empty and it is.
func contextCacheInvalidate(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	if _, err := q.Exec(ctx, contextCacheInvalidateSQL); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentCacheGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var result string
	err := q.QueryRow(ctx, agentCacheGetSQL, f[0], f[1]).Scan(&result)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{result}, nil
}

func agentCachePut(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentCachePutSQL, f[0], f[1], f[2]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- the web page cache ---------------------------------------------------------

const (
	// Reading a page marks it used, which is what the eviction order reads.
	// One statement, so a read cannot record a hit it did not serve.
	webPageGetSQL = `UPDATE web_page_cache
	                    SET last_used_at = now()
	                  WHERE url = $1
	              RETURNING body,
	                        EXTRACT(EPOCH FROM (now() - fetched_at))::bigint,
	                        pinned_addr`

	webPagePutSQL = `INSERT INTO web_page_cache (url, body, byte_len, pinned_addr,
	                                             fetched_at, last_used_at)
	                 VALUES ($1, $2, length($2), $3, now(), now())
	                 ON CONFLICT (url) DO UPDATE SET
	                     body         = EXCLUDED.body,
	                     byte_len     = EXCLUDED.byte_len,
	                     pinned_addr  = EXCLUDED.pinned_addr,
	                     fetched_at   = now(),
	                     last_used_at = now()`

	webPageDropSQL = `DELETE FROM web_page_cache WHERE url = $1`
)

func webPageGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	key, ok := canonicalURL(f[0])
	if !ok {
		return store.StatusMissing, nil, nil
	}
	var body, pinned string
	var age int64
	err := q.QueryRow(ctx, webPageGetSQL, key).Scan(&body, &age, &pinned)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{body, store.I64toa(age), pinned}, nil
}

func webPagePut(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	key, ok := canonicalURL(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, webPagePutSQL, key, f[1], f[2]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func webPageDrop(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	key, ok := canonicalURL(f[0])
	if !ok {
		return store.StatusMissing, nil, nil
	}
	return touchedOrMissing(ctx, q, webPageDropSQL, key)
}

// canonicalURL normalises a URL so two spellings of the same page share one
// cache entry. It reports false for a URL the cache must not key on at all.
//
// This touches no table: it is string handling that lives here because the wire
// routed it here, like the clarify scoring in the conversation family.
//
// It mirrors the C original, refusals included. A scheme that is not
// http/https, or an empty or oversized authority, is REFUSED rather than passed
// through: every caller treats that as "do not cache this page", and handing
// the raw string back would instead cache it under a key nothing else produces.
func canonicalURL(raw string) (string, bool) {
	sep := strings.Index(raw, "://")
	if sep < 0 || sep >= 8 {
		return "", false
	}
	scheme := strings.ToLower(raw[:sep])
	if scheme != "http" && scheme != "https" {
		return "", false
	}
	rest := raw[sep+3:]
	tail := ""
	if cut := strings.IndexAny(rest, "/?#"); cut >= 0 {
		rest, tail = rest[:cut], rest[cut:]
	}
	if len(rest) == 0 || len(rest) >= 256 {
		return "", false
	}
	host := strings.ToLower(rest)
	// Suppress a default port so :443 and the bare host are one entry. A colon
	// inside an IPv6 literal is not a port separator.
	if colon := strings.LastIndex(host, ":"); colon >= 0 && !strings.Contains(host[colon:], "]") {
		dflt := 80
		if scheme == "https" {
			dflt = 443
		}
		if leadingInt(host[colon+1:]) == dflt {
			host = host[:colon]
		}
	}
	// Path and query verbatim; the fragment is dropped -- it never goes on the
	// wire, so keying on it would store the same bytes twice.
	if hash := strings.IndexByte(tail, '#'); hash >= 0 {
		tail = tail[:hash]
	}
	if len(tail) > 2047 {
		tail = truncateBytesAtRune(tail, 2047)
	}
	if tail == "" {
		tail = "/"
	}
	return scheme + "://" + host + tail, true
}

// truncateBytesAtRune cuts s to at most max BYTES without splitting a rune.
//
// The bound has to stay in bytes -- it exists so a cache key cannot grow without
// limit, and the column behind it is sized in bytes. But slicing a Go string at
// a byte offset respects no character boundary, so a URL carrying a multi-byte
// character across the bound came out as invalid UTF-8. That is not a cosmetic
// truncation: PostgreSQL refuses invalid UTF-8 in a TEXT column, so the key
// could not be stored at all and the page simply failed to cache, with an
// encoding error nothing upstream would connect to a long URL.
//
// Reachable rather than theoretical -- with a leading "/" a 4-byte character
// splits exactly at 2047, and other path prefixes split 2- and 3-byte ones.
// Emoji and non-Latin scripts in URLs are ordinary.
//
// The cut backs off to the start of the straddling rune, so the result is up to
// three bytes short of the bound and always valid UTF-8.
func truncateBytesAtRune(s string, max int) string {
	if len(s) <= max {
		return s
	}
	cut := max
	for cut > 0 && !utf8.RuneStart(s[cut]) {
		cut--
	}
	return s[:cut]
}

// leadingInt reads the leading decimal run the way C's atoi() does: the C
// compared atoi(port) against the default, so ":443" and ":0443" both suppress.
func leadingInt(s string) int {
	n := 0
	for i := 0; i < len(s) && s[i] >= '0' && s[i] <= '9'; i++ {
		n = n*10 + int(s[i]-'0')
		if n > 1<<20 {
			return -1
		}
	}
	return n
}

// webPageCanonicalURL replies with TWO cells: the canonical form and the rc the
// C entry point returned. The catalog declares only the first, but the C server
// emitted both and the C client still reads both, so the wire is two wide.
func webPageCanonicalURL(_ context.Context, _ store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	canon, ok := canonicalURL(f[0])
	if !ok {
		return store.StatusOK, []string{"", "-1"}, nil
	}
	return store.StatusOK, []string{canon, "0"}, nil
}

// --- context snapshots ------------------------------------------------------------

const (
	contextSnapshotInsertSQL = `INSERT INTO context_snapshots
	                                (session_id, memory_id, relevance_score)
	                            VALUES ($1, $2, $3)`

	// How many memories have been sampled at least this many times.
	contextSnapshotCountMinSQL = `SELECT COUNT(*) FROM (
	                                  SELECT memory_id FROM context_snapshots
	                                   GROUP BY memory_id HAVING COUNT(*) >= $1) enough`

	contextSnapshotIDsMinSQL = `SELECT memory_id FROM context_snapshots
	                             GROUP BY memory_id HAVING COUNT(*) >= $1
	                             ORDER BY COUNT(*) DESC, memory_id
	                             LIMIT $2`

	contextSnapshotCountForMemorySQL = `SELECT COUNT(*) FROM context_snapshots
	                                     WHERE memory_id = $1`

	// The sessions a memory has been sampled in. DISTINCT with the ordering
	// column in the list, so it is a question PostgreSQL can answer.
	contextSnapshotSessionsSQL = `SELECT DISTINCT session_id FROM context_snapshots
	                               WHERE memory_id = $1
	                               ORDER BY session_id LIMIT $2`

	contextSnapshotHasMemorySQL = `SELECT EXISTS (
	                                   SELECT 1 FROM context_snapshots WHERE memory_id = $1)`

	contextSnapshotInsertTurnSQL = `INSERT INTO context_activation_events
	                                    (session_id, memory_id, relevance_score, turn_index)
	                                VALUES ($1, $2, $3, $4)`

	contextActivationAdvanceSQL = `INSERT INTO context_activation_turns (session_id, current_turn)
	                               VALUES ($1, 1)
	                               ON CONFLICT (session_id) DO UPDATE
	                                  SET current_turn = context_activation_turns.current_turn + 1
	                               RETURNING current_turn`

	// The activation state of one conversation: for each unit ever injected in
	// this session, the most recent turn it fired on. One query per turn rather
	// than one per candidate -- the gate in front of retrieval has to be much
	// cheaper than the retrieval it guards, and a per-candidate round trip is
	// not. Highest turn first so a truncated read keeps the rows that matter:
	// the recent ones are the only ones sticky and cooldown can still act on.
	contextSnapshotActivationSQL = `SELECT memory_id, MAX(turn_index) AS last_turn
	                                  FROM context_activation_events
	                                 WHERE session_id = $1
	                                 GROUP BY memory_id
	                                 ORDER BY last_turn DESC, memory_id
	                                 LIMIT $2`
)

func contextSnapshotInsertTurn(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	memoryID, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok || memoryID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	score, ok := store.Atof(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	turn, ok := store.Atoi64(f[3])
	if !ok || turn <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, contextSnapshotInsertTurnSQL, f[0], memoryID, score, turn); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// contextSnapshotActivation answers one conversation's activation state as
// "<memory_id> <last_turn>" cells, which the caller reads into its gate.
func contextSnapshotActivation(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var currentTurn int64
	if err := q.QueryRow(ctx, contextActivationAdvanceSQL, f[0]).Scan(&currentTurn); err != nil {
		return store.StatusFailed, nil, err
	}
	// Reserve one response cell for the out-of-band turn marker. The caller's
	// max is a bound on the whole reply, not only the state query.
	rows, err := q.Query(ctx, contextSnapshotActivationSQL, f[0], max-1)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	defer rows.Close()
	// Memory id zero is an out-of-band marker carrying the persisted turn. Real
	// memory ids are strictly positive, so this cannot collide with state.
	out := make([]string, 0, 17)
	out = append(out, "0 "+store.I64toa(currentTurn))
	for rows.Next() {
		var memoryID, lastTurn int64
		if err := rows.Scan(&memoryID, &lastTurn); err != nil {
			return store.StatusFailed, nil, err
		}
		out = append(out, store.I64toa(memoryID)+" "+store.I64toa(lastTurn))
	}
	if err := rows.Err(); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, out, nil
}

func contextSnapshotInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	memoryID, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok || memoryID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	score, ok := store.Atof(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, contextSnapshotInsertSQL, f[0], memoryID, score); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// oneCount answers with a single count taken with one bigint argument.
func oneCount(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		n, ok := store.Atoi64(f[0])
		if !ok || n < 0 {
			return store.StatusInvalid, nil, nil
		}
		var count int64
		if err := q.QueryRow(ctx, sql, n).Scan(&count); err != nil {
			return store.StatusFailed, nil, err
		}
		return store.StatusOK, []string{store.I64toa(count)}, nil
	}
}

func contextSnapshotIDsMinSamples(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	min, ok := store.Atoi64(f[0])
	if !ok || min < 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, contextSnapshotIDsMinSQL, min, max)
}

func contextSnapshotSessionsForMemory(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	memoryID, ok := store.Atoi64(f[0])
	if !ok || memoryID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collectStrings(ctx, q, contextSnapshotSessionsSQL, memoryID, max)
}

func contextSnapshotHasMemory(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	memoryID, ok := store.Atoi64(f[0])
	if !ok || memoryID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var present bool
	if err := q.QueryRow(ctx, contextSnapshotHasMemorySQL, memoryID).Scan(&present); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(present)}, nil
}

// --- decisions --------------------------------------------------------------------

const decisionRecordSQL = `INSERT INTO decisions (window_id, description) VALUES ($1, $2)`

func decisionRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	windowID, ok := store.Atoi64(f[0])
	if !ok || windowID <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	// f[2] is the caller's timestamp, which the store sets itself.
	if _, err := q.Exec(ctx, decisionRecordSQL, windowID, f[1]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- file snapshots -----------------------------------------------------------------

const fsnapColumns = `s.id, s.turn, s.session_id,
                      to_char(s.created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                      s.label, COUNT(e.id)`

const (
	fsnapCreateSQL = `INSERT INTO file_snapshots (session_id, turn, label)
	                  VALUES ($1, $2, $3) RETURNING id`

	// Get-or-create, as one statement.
	//
	// The C read, and inserted if the read found nothing -- so two callers
	// snapshotting the same turn both missed and both inserted, and the session
	// ended up with two snapshots of one turn. Only one of them would be
	// restored. ON CONFLICT makes the uniqueness constraint decide.
	fsnapGetOrCreateSQL = `INSERT INTO file_snapshots (session_id, turn, label)
	                       VALUES ($1, $2, $3)
	                       ON CONFLICT (session_id, turn, label) DO UPDATE
	                           SET session_id = EXCLUDED.session_id
	                       RETURNING id`

	fsnapListSQL = `SELECT ` + fsnapColumns + `
	                  FROM file_snapshots s
	                  LEFT JOIN file_snapshot_entries e ON e.snapshot_id = s.id
	                 WHERE s.session_id = $1
	                 GROUP BY s.id
	                 ORDER BY s.id DESC LIMIT $2`

	fsnapGetSQL = `SELECT ` + fsnapColumns + `
	                 FROM file_snapshots s
	                 LEFT JOIN file_snapshot_entries e ON e.snapshot_id = s.id
	                WHERE s.id = $1
	                GROUP BY s.id`

	// Keeping the newest `keep` snapshots for a session and dropping the rest.
	// The entries go with them through the cascade.
	fsnapPruneSQL = `DELETE FROM file_snapshots
	                  WHERE session_id = $1
	                    AND id NOT IN (SELECT id FROM file_snapshots
	                                    WHERE session_id = $1
	                                    ORDER BY id DESC LIMIT $2)`
)

func fsnapRow(scan func(...any) error) ([]string, error) {
	var id, turn, fileCount int64
	var sessionID, createdAt, label string
	if err := scan(&id, &turn, &sessionID, &createdAt, &label, &fileCount); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), store.I64toa(turn), sessionID, createdAt,
		label, store.I64toa(fileCount),
	}, nil
}

// fsnapTurnAndLabel reads the three fields both create operations take.
func fsnapTurnAndLabel(f []string) (sessionID string, turn int64, label string, ok bool) {
	if f[0] == "" {
		return "", 0, "", false
	}
	turn, ok = store.Atoi64(f[1])
	if !ok || turn < 0 {
		return "", 0, "", false
	}
	return f[0], turn, f[2], true
}

func fsnapCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sessionID, turn, label, ok := fsnapTurnAndLabel(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, fsnapCreateSQL, sessionID, turn, label).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func fsnapGetOrCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sessionID, turn, label, ok := fsnapTurnAndLabel(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, fsnapGetOrCreateSQL, sessionID, turn, label).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func fsnapList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, fsnapListSQL, fsnapCells, fsnapRow, f[0], max)
}

func fsnapGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reply, err := fsnapRow(func(dest ...any) error {
		return q.QueryRow(ctx, fsnapGetSQL, id).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func fsnapPrune(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	keep, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok || keep <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, fsnapPruneSQL, f[0], keep)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// --- the OSV advisory cache -------------------------------------------------------

// client_name is the first reply cell and is always empty: the C's row struct
// carried the field but no SELECT ever filled it, because the table has no such
// column. It is kept so the reply is the width the wire declares.
const osvColumns = `ecosystem, name, version, verdict, advisory_ids, checked_at`

const (
	osvCacheGetSQL = `SELECT ` + osvColumns + `
	                    FROM mcp_osv_cache
	                   WHERE ecosystem = $1 AND name = $2 AND version = $3
	                     AND checked_at > EXTRACT(EPOCH FROM now())::bigint - $4 * 3600`

	osvCacheUpsertSQL = `INSERT INTO mcp_osv_cache
	                         (ecosystem, name, version, verdict, advisory_ids, checked_at)
	                     VALUES ($1, $2, $3, $4, $5, EXTRACT(EPOCH FROM now())::bigint)
	                     ON CONFLICT (ecosystem, name, version) DO UPDATE SET
	                         verdict      = EXCLUDED.verdict,
	                         advisory_ids = EXCLUDED.advisory_ids,
	                         checked_at   = EXCLUDED.checked_at`

	osvCacheListSQL = `SELECT ` + osvColumns + `
	                     FROM mcp_osv_cache
	                     ORDER BY checked_at DESC, ecosystem, name, version
	                     LIMIT $1`
)

func osvRow(scan func(...any) error) ([]string, error) {
	var ecosystem, name, version, verdict, advisoryIDs string
	var checkedAt int64
	if err := scan(&ecosystem, &name, &version, &verdict, &advisoryIDs, &checkedAt); err != nil {
		return nil, err
	}
	return []string{
		"", // client_name: see the note above
		ecosystem, name, version, verdict, advisoryIDs,
		store.I64toa(checkedAt),
		// checked_at_text: the same instant, rendered.
		store.I64toa(checkedAt),
	}, nil
}

func osvCacheGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	ttl, ok := store.Atoi64(f[3])
	if !ok || ttl < 0 {
		return store.StatusInvalid, nil, nil
	}
	reply, err := osvRow(func(dest ...any) error {
		return q.QueryRow(ctx, osvCacheGetSQL, f[0], f[1], f[2], ttl).Scan(dest...)
	})
	if store.IsNoRows(err) {
		// Either nothing is cached or what is cached is too old. Both mean
		// "ask the advisory service", which is the only thing the caller does.
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func osvCacheUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[3] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, osvCacheUpsertSQL, f[0], f[1], f[2], f[3], f[4]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func osvCacheList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, osvCacheListSQL, osvCells, osvRow, max)
}

// osvAuditSQL records the check as an interaction event.
//
// The audit does not go to a table of its own: the C assembled a JSON payload
// and recorded it through the interaction-event path, which is the same table
// the agent-work family owns. Both live in this store, so the write is ordinary
// SQL; what does not cross the family boundary is a foreign key.
const osvAuditSQL = `INSERT INTO interaction_events
                         (session_id, event_type, actor, payload, outcome)
                     VALUES ('', 'mcp_package_check', 'system',
                             json_build_object(
                                 'client', $1::text, 'ecosystem', $2::text,
                                 'name', $3::text, 'version', $4::text,
                                 'verdict', $5::text, 'action', $6::text,
                                 'advisory_ids', $7::text)::text,
                             CASE WHEN $6 = 'block' THEN 'blocked' ELSE 'ok' END)`

// osvAudit records that a dependency check happened and what was decided.
//
// A blocked package is recorded with a "blocked" outcome, which is what makes
// the event findable later: the question anyone asks of this log is "what did
// we refuse to install", and an outcome of "ok" for a refusal would bury it.
func osvAudit(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, osvAuditSQL,
		f[0], f[1], f[2], f[3], f[4], f[5], f[6]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

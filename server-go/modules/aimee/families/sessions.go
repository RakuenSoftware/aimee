package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The sessions family: the server's sessions, per-agent transcripts, the
// write-path log, and the two webchat bindings.
const (
	EventSessions uint32 = 11782
	StageSessions uint32 = 6

	opServerSessionCreate        uint32 = 1
	opServerSessionGet           uint32 = 2
	opServerSessionSetOutcome    uint32 = 3
	opServerSessionDelete        uint32 = 4
	opServerSessionListRecent    uint32 = 5
	opServerSessionSearchByTitle uint32 = 6
	opServerSessionCount         uint32 = 7
	opServerSessionListExpired   uint32 = 8
	opServerSessionDeleteExpired uint32 = 9
	opPrimarySessionSave         uint32 = 10
	opPrimarySessionLoad         uint32 = 11
	opPrimarySessionDelete       uint32 = 12
	opPrimarySessionAllocRecent  uint32 = 13
	opPrimarySessionAllocSearch  uint32 = 14
	opPrimarySessionGetLatest    uint32 = 15
	opSessionWritePathRecord     uint32 = 16
	opSessionStaleReads          uint32 = 17
	opWebchatClaudeSessionGet    uint32 = 18
	opWebchatClaudeOwnedByOther  uint32 = 19
	opWebchatClaudeSessionBind   uint32 = 20
	opWebchatLiveSet             uint32 = 21
	opWebchatLiveGet             uint32 = 22
	opPersonaDeliveryClaim       uint32 = 23
	opPersonaDeliveryFinish      uint32 = 24
)

// Persona delivery states. The claim is an UPDATE guarded on unclaimed, so of
// two concurrent first requests exactly one sees a row change.
//
// The statements above spell these as literals because they are const strings;
// the names are here so the guards read as states rather than as numbers.
const (
	personaUnclaimed = 0
	personaDelivered = 1
	personaInFlight  = 2
)

// personaStates is what the schema's CHECK admits, asserted in the tests so the
// two cannot drift apart.
var personaStates = []int{personaUnclaimed, personaDelivered, personaInFlight}

const sessionsListMax = 512

// serverSessionColumns is the ten-cell row every server-session read answers
// with.
//
// The C's list and search queries selected only EIGHT of these and emitted ten
// cells, so `source` and `chat_key` were structurally always blank on a list --
// the query had simply not been updated when the columns were added. Selecting
// them is the fix; the wire already had the room.
//
// EXCEPT IT IS NOT THE FIX, and the paragraph above is left standing because
// what it got wrong is worth more than a tidy correction.
//
// Selecting a column changes what a reader receives only if something writes
// it. Of these ten, FOUR have no writer anywhere in the module: title, source,
// chat_key and claude_session_id on this table are never in an INSERT column
// list and never on the left of an UPDATE ... SET. So they were blank before
// the change, and they are blank after it, and the only thing that moved is
// that the query now says so explicitly.
//
// The diagnosis was right about the symptom and looked one layer too high for
// the cause: the read side was genuinely wrong AND the write side was wrong,
// and repairing the visible half made the remaining half harder to see, because
// the column now appears in the query where a reader would go looking.
//
// (claude_session_id is written -- on webchat_claude_sessions, a different table
// that happens to share the name. Same column name, different row: exactly the
// coincidence that makes this class hard to grep for.)
//
// Left as it stands rather than repaired here, because giving four columns
// writers means deciding what each should contain, which is not a decision this
// file gets to make quietly. scripts/survey-unwritten-columns.py finds the
// whole set; there are 27 across the schema.
const serverSessionColumns = `id, client_type, principal, title,
	    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	    to_char(last_activity_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	    claude_session_id, outcome, source, chat_key`

// TITLE HAS FOUR READERS AND NO WRITER, and has had none since before this port.
//
// server_session_get, _list_recent, _search_by_title and insights_top_sessions
// all return it. No operation accepts it: it is not a request field anywhere in
// the catalog, and no statement here inserts or updates it. The C did the same
// -- its INSERT wrote an empty string into the column -- so this is inherited
// rather than dropped in translation, and faithfully so.
//
// The consequence is a user-facing command that cannot work.
// src/cmd_session_history.c:175 calls db1_server_session_search_by_title from a
// session search, and the column it searches can only ever hold the empty
// default, so that leg returns nothing for every pattern.
//
// AND IT IS TESTED. src/tests/test_cmd_session.c inserts sessions with titles
// by writing SQLite directly, then asserts the search finds them. That proves
// the SELECT is correct and nothing about the feature, because it constructs
// state no production path can produce. A test that builds its own fixtures
// around a missing writer will pass forever.
//
// NOT FIXED HERE because the fix is a decision rather than a repair: either
// something starts setting a title -- which is a new operation and a question
// about what a session's title should say -- or the search and the column go,
// which removes a command's capability. Both are the owner's call. Writing it
// down where the create statement is, so the next person to add a column notices
// the pattern.
const (
	serverSessionCreateSQL = `INSERT INTO server_sessions (id, client_type, principal)
	                          VALUES ($1, $2, $3)`

	serverSessionGetSQL = `SELECT ` + serverSessionColumns +
		` FROM server_sessions WHERE id = $1`

	serverSessionSetOutcomeSQL = `UPDATE server_sessions SET outcome = $2 WHERE id = $1`

	serverSessionDeleteSQL = `DELETE FROM server_sessions WHERE id = $1`

	serverSessionListSQL = `SELECT ` + serverSessionColumns + `
	                          FROM server_sessions
	                         ORDER BY last_activity_at DESC, id
	                         LIMIT $1`

	// ILIKE, not LIKE. The pattern comes from a person typing into a search
	// box, and SQLite's LIKE was case-insensitive, so a case-sensitive LIKE
	// here would quietly stop finding sessions that used to match.
	serverSessionSearchSQL = `SELECT ` + serverSessionColumns + `
	                            FROM server_sessions
	                           WHERE title ILIKE $1
	                           ORDER BY last_activity_at DESC, id
	                           LIMIT $2`

	serverSessionCountAllSQL   = `SELECT count(*) FROM server_sessions`
	serverSessionCountSinceSQL = `SELECT count(*) FROM server_sessions WHERE created_at >= $1`

	// The threshold is a PARAMETER. The C built these two statements with
	// snprintf, splicing the number into the SQL text, because a TEXT timestamp
	// column left it no interval arithmetic to do.
	serverSessionListExpiredSQL = `SELECT id FROM server_sessions
	                                WHERE created_at <= now() - make_interval(secs => $1)
	                                ORDER BY created_at, id
	                                LIMIT $2`

	serverSessionDeleteExpiredSQL = `DELETE FROM server_sessions
	                                  WHERE created_at <= now() - make_interval(secs => $1)`

	personaClaimSQL = `UPDATE server_sessions
	                      SET persona_delivery_state = 2, last_activity_at = now()
	                    WHERE id = $1 AND persona_delivery_state = 0`

	personaStateSQL = `SELECT persona_delivery_state FROM server_sessions WHERE id = $1`

	personaFinishSQL = `UPDATE server_sessions
	                       SET persona_delivery_state = $2, last_activity_at = now()
	                     WHERE id = $1 AND persona_delivery_state = 2`

	primarySessionSaveSQL = `INSERT INTO primary_sessions
	                             (session_id, agent_name, provider, messages_json)
	                         VALUES ($1, $2, $3, $4)
	                         ON CONFLICT (session_id, agent_name, provider)
	                         DO UPDATE SET messages_json = EXCLUDED.messages_json,
	                                       updated_at = now()`

	primarySessionLoadSQL = `SELECT messages_json FROM primary_sessions
	                          WHERE session_id = $1 AND agent_name = $2 AND provider = $3`

	primarySessionDeleteSQL = `DELETE FROM primary_sessions
	                            WHERE session_id = $1 AND agent_name = $2 AND provider = $3`

	primarySessionColumns = `session_id, agent_name, provider, messages_json,
	        to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	        to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')`

	primaryAllocRecentSQL = `SELECT ` + primarySessionColumns + `
	                           FROM primary_sessions
	                          ORDER BY updated_at DESC, session_id
	                          LIMIT $1`

	// Four columns, because the caller is searching for a transcript and any of
	// these is how a person recognises one. The wire carries a RAW query, not a
	// pattern: the module wraps it, which is what the C did.
	primaryAllocSearchSQL = `SELECT ` + primarySessionColumns + `
	                           FROM primary_sessions
	                          WHERE messages_json ILIKE $1 ESCAPE '\'
	                             OR session_id    ILIKE $1 ESCAPE '\'
	                             OR agent_name    ILIKE $1 ESCAPE '\'
	                             OR provider      ILIKE $1 ESCAPE '\'
	                          ORDER BY updated_at DESC, session_id
	                          LIMIT $2`

	primaryGetLatestSQL = `SELECT ` + primarySessionColumns + `
	                         FROM primary_sessions
	                        WHERE session_id = $1
	                        ORDER BY updated_at DESC, agent_name, provider
	                        LIMIT 1`

	// seq is allocated from the current maximum so the caller does not track a
	// per-session counter. The primary key makes a duplicate impossible.
	writePathRecordSQL = `INSERT INTO session_state_write_paths (session_id, seq, path)
	                      VALUES ($1,
	                              coalesce((SELECT max(seq) + 1 FROM session_state_write_paths
	                                         WHERE session_id = $1), 0),
	                              $2)`

	// DISTINCT because both sides can repeat a path across sequences.
	staleReadsSQL = `SELECT DISTINCT r.path
	                   FROM session_state_read_paths r
	                   JOIN session_state_write_paths w ON r.path = w.path
	                  WHERE r.session_id = $1 AND w.session_id = $2
	                  ORDER BY r.path
	                  LIMIT $3`

	// principal is attribution, never a namespace: a tab is identified by its
	// aimee_session_id alone, and the C passed principal in and then explicitly
	// ignored it at every read.
	webchatClaudeGetSQL = `SELECT claude_session_id FROM webchat_claude_sessions
	                        WHERE aimee_session_id = $1
	                        ORDER BY updated_at DESC
	                        LIMIT 1`

	webchatOwnedByOtherSQL = `SELECT 1 FROM webchat_claude_sessions
	                           WHERE claude_session_id = $1 AND aimee_session_id <> $2
	                           LIMIT 1`

	webchatBindUpdateSQL = `UPDATE webchat_claude_sessions
	                           SET claude_session_id = $1, updated_at = now()
	                         WHERE aimee_session_id = $2`

	webchatBindInsertSQL = `INSERT INTO webchat_claude_sessions
	                            (principal, aimee_session_id, claude_session_id)
	                        VALUES ($1, $2, $3)`

	webchatLiveSetSQL = `INSERT INTO webchat_live (session_id, turn_id, rev, text, status)
	                     VALUES ($1, $2, 1, $3, $4)
	                     ON CONFLICT (session_id) DO UPDATE SET
	                         turn_id = EXCLUDED.turn_id,
	                         rev = webchat_live.rev + 1,
	                         text = EXCLUDED.text,
	                         status = EXCLUDED.status,
	                         updated_at = now()`

	webchatLiveGetSQL = `SELECT turn_id, text, status, rev FROM webchat_live
	                      WHERE session_id = $1 AND rev > $2`
)

// serverSessionRow reads the ten cells of a server-session row.
func serverSessionRow(scan func(...any) error) ([]string, error) {
	cells := make([]string, 10)
	dest := make([]any, 10)
	for i := range cells {
		dest[i] = &cells[i]
	}
	if err := scan(dest...); err != nil {
		return nil, err
	}
	return cells, nil
}

// primarySessionRow reads the six cells of a primary-session row.
func primarySessionRow(scan func(...any) error) ([]string, error) {
	cells := make([]string, 6)
	dest := make([]any, 6)
	for i := range cells {
		dest[i] = &cells[i]
	}
	if err := scan(dest...); err != nil {
		return nil, err
	}
	return cells, nil
}

// collect runs a list query and flattens the rows.
func collect(ctx context.Context, q store.Queryer, sql string, width int,
	read func(func(...any) error) ([]string, error), args ...any) (uint32, []string, error) {
	rows, err := q.Query(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	cells := make([]string, 0, width*8)
	for rows.Next() {
		row, err := read(rows.Scan)
		if err != nil {
			return 0, nil, err
		}
		cells = append(cells, row...)
	}
	if err := rows.Err(); err != nil {
		// Reported rather than returning a short list: on this wire a partial
		// list is indistinguishable from a complete one.
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

func boundedMax(field string) (int, bool) {
	max, ok := store.Atoi(field)
	if !ok || max <= 0 || max > sessionsListMax {
		return 0, false
	}
	return max, true
}

// --- server sessions ----------------------------------------------------------

func serverSessionCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, serverSessionCreateSQL, f[0], f[1], f[2]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func serverSessionGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	cells, err := serverSessionRow(q.QueryRow(ctx, serverSessionGetSQL, f[0]).Scan)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

func serverSessionSetOutcome(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, serverSessionSetOutcomeSQL, f[0], f[1]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func serverSessionDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, serverSessionDeleteSQL, f[0]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func serverSessionListRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, serverSessionListSQL, 10, serverSessionRow, max)
}

func serverSessionSearchByTitle(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// The pattern is the caller's, wildcards and all. Unlike the transcript
	// search below, this operation's wire carries a PATTERN rather than a raw
	// query -- the C bound it unwrapped -- so composing it is the caller's job.
	return collect(ctx, q, serverSessionSearchSQL, 10, serverSessionRow, f[0], max)
}

// serverSessionCount is op 7. An empty `since` means "all time" rather than
// "since the epoch", which is why it is two statements and not a coalesce.
func serverSessionCount(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	var n int64
	var err error
	if f[0] == "" {
		err = q.QueryRow(ctx, serverSessionCountAllSQL).Scan(&n)
	} else {
		err = q.QueryRow(ctx, serverSessionCountSinceSQL, f[0]).Scan(&n)
	}
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(n)}, nil
}

func serverSessionListExpired(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	threshold, okThreshold := store.Atoi(f[0])
	max, okMax := boundedMax(f[1])
	if !okThreshold || threshold < 0 || !okMax {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, serverSessionListExpiredSQL, 1,
		func(scan func(...any) error) ([]string, error) {
			var id string
			if err := scan(&id); err != nil {
				return nil, err
			}
			return []string{id}, nil
		}, threshold, max)
}

// serverSessionDeleteExpired is op 9 and answers with how many it removed.
func serverSessionDeleteExpired(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	threshold, ok := store.Atoi(f[0])
	if !ok || threshold < 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, serverSessionDeleteExpiredSQL, threshold)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// --- persona delivery ----------------------------------------------------------

// personaDeliveryClaim is op 23: take the right to deliver the persona once.
//
// The claim is a guarded UPDATE, not a read then a write. Two connections
// opening at the same instant both pass a read; only one of them changes a row.
//
// 1 means this caller holds the claim. 0 means somebody else already holds or
// completed it. A session that does not exist is neither, and is a failure.
func personaDeliveryClaim(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, personaClaimSQL, f[0])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() == 1 {
		return store.StatusOK, []string{"1"}, nil
	}
	var state int
	switch err := q.QueryRow(ctx, personaStateSQL, f[0]).Scan(&state); {
	case store.IsNoRows(err):
		// No such session: not "already handled".
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if state >= personaDelivered {
		return store.StatusOK, []string{"0"}, nil
	}
	return store.StatusFailed, nil, nil
}

// personaDeliveryFinish is op 24: release a claim.
//
// delivered == 0 returns the session to unclaimed so a later request retries,
// rather than the persona being lost because one request failed after claiming.
func personaDeliveryFinish(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	delivered, ok := store.Atoi(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	state := personaUnclaimed
	if delivered != 0 {
		state = personaDelivered
	}
	tag, err := q.Exec(ctx, personaFinishSQL, f[0], state)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		// The session was not in flight, so this caller did not hold the claim.
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

// --- primary sessions ----------------------------------------------------------

func primarySessionSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, primarySessionSaveSQL, f[0], f[1], f[2], f[3]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func primarySessionLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	var messages string
	switch err := q.QueryRow(ctx, primarySessionLoadSQL, f[0], f[1], f[2]).Scan(&messages); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{messages}, nil
}

func primarySessionDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, primarySessionDeleteSQL, f[0], f[1], f[2]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func primarySessionAllocRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, primaryAllocRecentSQL, 6, primarySessionRow, max)
}

// primarySessionAllocSearch is op 14: find transcripts by free text.
//
// f[0] is the user's raw query, NOT a pattern -- it is wrapped here and escaped,
// so a query containing % matches a literal % rather than everything.
func primarySessionAllocSearch(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, primaryAllocSearchSQL, 6, primarySessionRow, likeContains(f[0]), max)
}

func primarySessionGetLatest(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	cells, err := primarySessionRow(q.QueryRow(ctx, primaryGetLatestSQL, f[0]).Scan)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// --- write paths ----------------------------------------------------------------

func sessionWritePathRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, writePathRecordSQL, f[0], f[1]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// sessionStaleReads is op 17: paths the parent read that the child has written.
func sessionStaleReads(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[2])
	if f[0] == "" || f[1] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, staleReadsSQL, 1,
		func(scan func(...any) error) ([]string, error) {
			var path string
			if err := scan(&path); err != nil {
				return nil, err
			}
			return []string{path}, nil
		}, f[0], f[1], max)
}

// --- webchat ---------------------------------------------------------------------

func webchatClaudeSessionGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	var claudeID string
	switch err := q.QueryRow(ctx, webchatClaudeGetSQL, f[1]).Scan(&claudeID); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if claudeID == "" {
		// A row with no binding is the same answer as no row.
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{claudeID, "0"}, nil
}

// webchatOwnedByOther is op 19: does another tab already own this id.
//
// It answers 0 on failure rather than reporting one, because the C did and
// because the caller uses it as a guard -- an error that read as "not owned"
// would be the wrong way to fail, but reporting a status the caller does not
// branch on would be worse.
func webchatOwnedByOther(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[2] == "" {
		return store.StatusOK, []string{"0"}, nil
	}
	var one int
	switch err := q.QueryRow(ctx, webchatOwnedByOtherSQL, f[2], f[1]).Scan(&one); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{"1"}, nil
}

// webchatClaudeSessionBind is op 20.
//
// A binding is never hijacked: if another tab already owns this claude session
// id, the bind is refused rather than repointing it. Existing databases can
// hold one tab under several historical principals, so the update touches every
// copy -- principal is attribution, not a namespace.
func webchatClaudeSessionBind(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	principal, aimeeID, claudeID := f[0], f[1], f[2]
	if aimeeID == "" || claudeID == "" {
		return store.StatusInvalid, nil, nil
	}
	var one int
	switch err := q.QueryRow(ctx, webchatOwnedByOtherSQL, claudeID, aimeeID).Scan(&one); {
	case err == nil:
		return store.StatusFailed, nil, nil
	case !store.IsNoRows(err):
		return 0, nil, err
	}
	tag, err := q.Exec(ctx, webchatBindUpdateSQL, claudeID, aimeeID)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() > 0 {
		return store.StatusOK, nil, nil
	}
	if _, err := q.Exec(ctx, webchatBindInsertSQL, principal, aimeeID, claudeID); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func webchatLiveSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, webchatLiveSetSQL, f[0], f[1], f[2], f[3]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// webchatLiveGet is op 22: the live turn, but only if it has advanced past the
// revision the poller already has.
func webchatLiveGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	sinceRev, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	var turnID, text, status string
	var rev int64
	switch err := q.QueryRow(ctx, webchatLiveGetSQL, f[0], sinceRev).
		Scan(&turnID, &text, &status, &rev); {
	case store.IsNoRows(err):
		// Nothing new. Not a failure: the poller asked whether the row had
		// moved and it had not.
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{turnID, text, status, store.I64toa(rev)}, nil
}

// Sessions is the family, ready to be bound to kind 11782.
var Sessions = store.Family{
	Name:  "sessions",
	Event: EventSessions,
	Stage: StageSessions,
	Ops: map[uint32]store.Op{
		opServerSessionCreate:        {Name: "server_session_create", Args: 3, Tx: true, Run: serverSessionCreate},
		opServerSessionGet:           {Name: "server_session_get", Cells: 10, Args: 1, Run: serverSessionGet},
		opServerSessionSetOutcome:    {Name: "server_session_set_outcome", Args: 2, Tx: true, Run: serverSessionSetOutcome},
		opServerSessionDelete:        {Name: "server_session_delete", Args: 1, Tx: true, Run: serverSessionDelete},
		opServerSessionListRecent:    {Name: "server_session_list_recent", Cells: 10, Args: 1, Run: serverSessionListRecent},
		opServerSessionSearchByTitle: {Name: "server_session_search_by_title", Cells: 10, Args: 2, Run: serverSessionSearchByTitle},
		opServerSessionCount:         {Name: "server_session_count", Args: 1, Run: serverSessionCount},
		opServerSessionListExpired:   {Name: "server_session_list_expired", Cells: 1, Args: 2, Run: serverSessionListExpired},
		opServerSessionDeleteExpired: {Name: "server_session_delete_expired", Args: 1, Tx: true, Run: serverSessionDeleteExpired},
		opPrimarySessionSave:         {Name: "primary_session_save", Args: 4, Tx: true, Run: primarySessionSave},
		opPrimarySessionLoad:         {Name: "primary_session_load", Args: 3, Run: primarySessionLoad},
		opPrimarySessionDelete:       {Name: "primary_session_delete", Args: 3, Tx: true, Run: primarySessionDelete},
		opPrimarySessionAllocRecent:  {Name: "primary_session_alloc_recent", Cells: 6, Args: 1, Run: primarySessionAllocRecent},
		opPrimarySessionAllocSearch:  {Name: "primary_session_alloc_search", Cells: 6, Args: 2, Run: primarySessionAllocSearch},
		opPrimarySessionGetLatest:    {Name: "primary_session_get_latest", Cells: 6, Args: 1, Run: primarySessionGetLatest},
		opSessionWritePathRecord:     {Name: "session_write_path_record", Args: 2, Tx: true, Run: sessionWritePathRecord},
		opSessionStaleReads:          {Name: "session_stale_reads", Cells: 1, Args: 3, Run: sessionStaleReads},
		opWebchatClaudeSessionGet:    {Name: "webchat_claude_session_get", Cells: 2, Args: 2, Run: webchatClaudeSessionGet},
		opWebchatClaudeOwnedByOther:  {Name: "webchat_claude_session_owned_by_other", Args: 3, Run: webchatOwnedByOther},
		opWebchatClaudeSessionBind:   {Name: "webchat_claude_session_bind", Args: 3, Tx: true, Run: webchatClaudeSessionBind},
		opWebchatLiveSet:             {Name: "webchat_live_set", Args: 4, Tx: true, Run: webchatLiveSet},
		opWebchatLiveGet:             {Name: "webchat_live_get", Cells: 4, Args: 2, Run: webchatLiveGet},
		opPersonaDeliveryClaim:       {Name: "server_session_persona_delivery_claim", Args: 1, Tx: true, Run: personaDeliveryClaim},
		opPersonaDeliveryFinish:      {Name: "server_session_persona_delivery_finish", Args: 2, Tx: true, Run: personaDeliveryFinish},
	},
}

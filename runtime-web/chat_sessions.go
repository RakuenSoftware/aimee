package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// Deployment-wide chat-session persistence. A "session" here is one browser tab's
// conversation: identified by the stable id the SPA sends as aimee_session_id.
// The provider-native id emitted by the "session" stream event is separate and
// is persisted only as resume metadata. username columns are retained as actor
// attribution for existing databases; they never scope visibility or ownership.

type ctxKey string

const ctxUsername ctxKey = "webchat_username"

// currentUser returns the authenticated actor injected by requireAuth, or "" if
// the request was not authenticated. It is authorization/audit identity, never
// a state namespace.
func currentUser(r *http.Request) string {
	u, _ := r.Context().Value(ctxUsername).(string)
	return u
}

// withUser returns a shallow copy of r carrying the resolved actor, used by
// requireAuth for authorization and attribution.
func withUser(r *http.Request, username string) *http.Request {
	return r.WithContext(context.WithValue(r.Context(), ctxUsername, username))
}

var chatSessionMigrations = []string{
	`CREATE TABLE IF NOT EXISTS chat_sessions (
		id          TEXT PRIMARY KEY,
		username    TEXT NOT NULL,
		title       TEXT NOT NULL DEFAULT '',
		cwd         TEXT NOT NULL DEFAULT '',
		created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
		last_active TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
	)`,
	`CREATE INDEX IF NOT EXISTS idx_chat_sessions_user ON chat_sessions(username, last_active DESC)`,
	`CREATE TABLE IF NOT EXISTS chat_session_runtime (
		session_id          TEXT PRIMARY KEY,
		username            TEXT NOT NULL,
		provider_session_id TEXT NOT NULL DEFAULT ''
	)`,
	`CREATE TABLE IF NOT EXISTS chat_session_messages (
		id         INTEGER PRIMARY KEY AUTOINCREMENT,
		session_id TEXT NOT NULL,
		username   TEXT NOT NULL,
		role       TEXT NOT NULL,
		text       TEXT NOT NULL,
		created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
	)`,
	`CREATE INDEX IF NOT EXISTS idx_chat_session_messages
		ON chat_session_messages(username, session_id, id)`,
	`CREATE TABLE IF NOT EXISTS chat_session_schema_migrations (
		version INTEGER PRIMARY KEY
	)`,
}

func applyChatSessionMigrations(db *sql.DB) error {
	for _, m := range chatSessionMigrations {
		if _, err := db.Exec(m); err != nil {
			return err
		}
	}
	return migrateLegacyChatSessionRuntime(db)
}

// Version 1 upgrades rows written by the old webchat implementation. That
// implementation replaced the stable browser id with the provider-emitted id
// before inserting chat_sessions, so each pre-existing row id is trusted
// provider resume metadata already captured by the server. Snapshot those rows
// exactly once; rerunning this migration must never classify a newly-created
// stable web id as a provider id.
func migrateLegacyChatSessionRuntime(db *sql.DB) error {
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var applied int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM chat_session_schema_migrations
		WHERE version = 1`).Scan(&applied); err != nil {
		return err
	}
	if applied == 0 {
		if _, err := tx.Exec(`INSERT OR IGNORE INTO chat_session_runtime
			(session_id, username, provider_session_id)
			SELECT id, username, id FROM chat_sessions`); err != nil {
			return err
		}
		if _, err := tx.Exec(`INSERT INTO chat_session_schema_migrations (version) VALUES (1)`); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// chatSession is the durable record of one tab's session in this environment.
type chatSession struct {
	ID                string        `json:"id"`
	Title             string        `json:"title"`
	Cwd               string        `json:"cwd,omitempty"`
	ProviderSessionID string        `json:"provider_session_id,omitempty"`
	Messages          []chatMessage `json:"messages"`
	CreatedAt         string        `json:"created_at"`
	LastActive        string        `json:"last_active"`
}

type chatMessage struct {
	Role string `json:"role"`
	Text string `json:"text"`
}

const sessionTitleMaxLen = 80

// deriveTitle builds a short, single-line title from the first user message.
func deriveTitle(message string) string {
	title := strings.TrimSpace(message)
	if i := strings.IndexAny(title, "\r\n"); i >= 0 {
		title = strings.TrimSpace(title[:i])
	}
	if len(title) > sessionTitleMaxLen {
		title = strings.TrimSpace(title[:sessionTitleMaxLen]) + "…"
	}
	return title
}

// touchChatSession records (or refreshes) an environment session from the chat-send
// path. It is best-effort: a nil db, an empty user, or an empty id is a no-op,
// and any DB error is returned for the caller to log without failing the chat.
// The session's last_active is bumped; cwd is updated when provided; the title
// is auto-derived from the first message only while still empty (so an explicit
// rename via the session endpoint is never clobbered by later turns).
func (s *server) touchChatSession(username, id, cwd, firstMessage string) error {
	if s == nil || s.db == nil || username == "" || id == "" {
		return nil
	}
	now := time.Now().UTC().Format(time.RFC3339)
	title := deriveTitle(firstMessage)
	_, err := s.db.Exec(`
		INSERT INTO chat_sessions (id, username, title, cwd, created_at, last_active)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			last_active = excluded.last_active,
			cwd   = CASE WHEN excluded.cwd != ''        THEN excluded.cwd   ELSE chat_sessions.cwd   END,
			title = CASE WHEN chat_sessions.title = ''  THEN excluded.title ELSE chat_sessions.title END`,
		id, username, title, cwd, now, now)
	return err
}

// chatSessionOwned reports whether id exists in this environment. The retained
// username parameter is actor attribution/API compatibility, not ownership.
func (s *server) chatSessionOwned(username, id string) (owned, exists bool, err error) {
	if s == nil || s.db == nil || username == "" || id == "" {
		return false, false, nil
	}
	var one int
	err = s.db.QueryRow(`SELECT 1 FROM chat_sessions WHERE id = ?`, id).Scan(&one)
	switch err {
	case nil:
		return true, true, nil
	case sql.ErrNoRows:
		return false, false, nil
	default:
		return false, false, err
	}
}

// setChatSessionRuntime persists the provider-native thread/session id needed to
// resume a restored chat on another computer. The stable web session id remains
// the chat_sessions key; provider ids must never replace it.
func (s *server) setChatSessionRuntime(username, id, providerSessionID string) error {
	if s == nil || s.db == nil || username == "" || id == "" {
		return nil
	}
	_, err := s.db.Exec(`
		INSERT INTO chat_session_runtime (session_id, username, provider_session_id)
		SELECT ?, ?, ?
		 WHERE EXISTS (SELECT 1 FROM chat_sessions WHERE id = ?)
		ON CONFLICT(session_id) DO UPDATE SET
			provider_session_id = CASE
				WHEN excluded.provider_session_id != '' THEN excluded.provider_session_id
				ELSE chat_session_runtime.provider_session_id END`,
		id, username, providerSessionID, id)
	return err
}

// chatSessionProviderID returns resume metadata only when it is already bound
// to the authenticated user's stable session. Provider-native ids are opaque
// bearer-like handles, so callers must never substitute a value supplied by the
// browser here.
func (s *server) chatSessionProviderID(username, id string) (string, error) {
	if s == nil || s.db == nil || username == "" || id == "" {
		return "", nil
	}
	var providerSessionID string
	err := s.db.QueryRow(`SELECT provider_session_id FROM chat_session_runtime
		WHERE session_id = ?`, id).Scan(&providerSessionID)
	switch err {
	case nil:
		return providerSessionID, nil
	case sql.ErrNoRows:
		return "", nil
	default:
		return "", err
	}
}

// upsertChatSessionWithLegacyRuntime atomically creates/updates a stable target
// row and moves a provider binding from an old provider-keyed row. The browser
// supplies only the alias selector; the provider id itself comes from the
// server's one-shot legacy migration and must equal that owned alias row.
// Invalid aliases roll the whole operation back, so a rejected migration cannot
// strand a ghost target session.
func (s *server) upsertChatSessionWithLegacyRuntime(username, targetID, aliasID, title, cwd, now string) (bool, error) {
	if s == nil || s.db == nil || username == "" || targetID == "" || aliasID == "" || targetID == aliasID {
		return false, nil
	}
	tx, err := s.db.Begin()
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	var providerSessionID string
	err = tx.QueryRow(`SELECT runtime.provider_session_id
		FROM chat_sessions AS legacy
		JOIN chat_session_runtime AS runtime
		  ON runtime.session_id = legacy.id
		WHERE legacy.id = ?
		  AND runtime.provider_session_id = legacy.id`, aliasID).Scan(&providerSessionID)
	if err == sql.ErrNoRows {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if _, err := tx.Exec(`INSERT INTO chat_sessions
		(id, username, title, cwd, created_at, last_active)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			last_active = excluded.last_active,
			cwd   = CASE WHEN excluded.cwd   != '' THEN excluded.cwd   ELSE chat_sessions.cwd   END,
			title = CASE WHEN excluded.title != '' THEN excluded.title ELSE chat_sessions.title END`,
		targetID, username, title, cwd, now, now); err != nil {
		return false, err
	}
	result, err := tx.Exec(`INSERT INTO chat_session_runtime
		(session_id, username, provider_session_id)
		SELECT ?, ?, ? WHERE EXISTS
		(SELECT 1 FROM chat_sessions WHERE id = ?)
		ON CONFLICT(session_id) DO UPDATE SET
			provider_session_id = CASE
				WHEN chat_session_runtime.provider_session_id = '' THEN excluded.provider_session_id
				ELSE chat_session_runtime.provider_session_id END`,
		targetID, username, providerSessionID, targetID)
	if err != nil {
		return false, err
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return false, err
	}
	if changed == 0 {
		return false, nil
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}

func (s *server) appendChatMessage(username, id, role, text string) error {
	if s == nil || s.db == nil || username == "" || id == "" || text == "" {
		return nil
	}
	if role != "user" && role != "assistant" && role != "narration" {
		return nil
	}
	_, err := s.db.Exec(`
		INSERT INTO chat_session_messages (session_id, username, role, text)
		SELECT ?, ?, ?, ?
		 WHERE EXISTS (SELECT 1 FROM chat_sessions WHERE id = ?)`,
		id, username, role, text, id)
	return err
}

const (
	chatSessionSeedMaxMessages = 2000
	chatSessionSeedMaxBytes    = 8 * 1024 * 1024
)

// seedChatMessages imports a legacy browser-only transcript once. Existing
// server history always wins, which prevents a stale browser from overwriting a
// conversation that another browser has advanced.
func (s *server) seedChatMessages(username, id string, messages []chatMessage) error {
	if s == nil || s.db == nil || username == "" || id == "" || len(messages) == 0 {
		return nil
	}
	if len(messages) > chatSessionSeedMaxMessages {
		return fmt.Errorf("too many messages")
	}
	total := 0
	for _, msg := range messages {
		if msg.Role != "user" && msg.Role != "assistant" && msg.Role != "narration" {
			return fmt.Errorf("invalid message role")
		}
		total += len(msg.Text)
		if total > chatSessionSeedMaxBytes {
			return fmt.Errorf("transcript too large")
		}
	}

	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var count int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM chat_session_messages
		WHERE session_id = ?`, id).Scan(&count); err != nil {
		return err
	}
	if count != 0 {
		return tx.Commit()
	}
	stmt, err := tx.Prepare(`INSERT INTO chat_session_messages
		(session_id, username, role, text)
		SELECT ?, ?, ?, ? WHERE EXISTS
		(SELECT 1 FROM chat_sessions WHERE id = ?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for _, msg := range messages {
		if msg.Text == "" {
			continue
		}
		if _, err := stmt.Exec(id, username, msg.Role, msg.Text, id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (s *server) loadChatSessionDetails(username string, cs *chatSession) error {
	if s == nil || s.db == nil || cs == nil {
		return nil
	}
	cs.Messages = []chatMessage{}
	_ = s.db.QueryRow(`SELECT provider_session_id FROM chat_session_runtime
		WHERE session_id = ?`, cs.ID).Scan(&cs.ProviderSessionID)
	rows, err := s.db.Query(`SELECT role, text FROM (
		SELECT id, role, text FROM chat_session_messages
		 WHERE session_id = ? ORDER BY id DESC LIMIT ?
	) ORDER BY id`, cs.ID, chatSessionSeedMaxMessages)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var msg chatMessage
		if err := rows.Scan(&msg.Role, &msg.Text); err != nil {
			return err
		}
		cs.Messages = append(cs.Messages, msg)
	}
	return rows.Err()
}

// getChatSession loads one shared environment session, or (nil, nil) if absent.
func (s *server) getChatSession(username, id string) (*chatSession, error) {
	if s == nil || s.db == nil || username == "" || id == "" {
		return nil, nil
	}
	row := s.db.QueryRow(
		`SELECT id, title, cwd, created_at, last_active
		   FROM chat_sessions WHERE id = ?`, id)
	var cs chatSession
	switch err := row.Scan(&cs.ID, &cs.Title, &cs.Cwd, &cs.CreatedAt, &cs.LastActive); err {
	case nil:
		if err := s.loadChatSessionDetails(username, &cs); err != nil {
			return nil, err
		}
		return &cs, nil
	case sql.ErrNoRows:
		return nil, nil
	default:
		return nil, err
	}
}

// listChatSessions returns the environment's sessions, most-recently-active first.
func (s *server) listChatSessions(username string) ([]chatSession, error) {
	out := []chatSession{}
	if s == nil || s.db == nil || username == "" {
		return out, nil
	}
	rows, err := s.db.Query(
		`SELECT id, title, cwd, created_at, last_active
		   FROM chat_sessions
		  ORDER BY last_active DESC LIMIT 200`)
	if err != nil {
		return out, err
	}
	defer rows.Close()
	for rows.Next() {
		var cs chatSession
		if err := rows.Scan(&cs.ID, &cs.Title, &cs.Cwd, &cs.CreatedAt, &cs.LastActive); err != nil {
			return out, err
		}
		out = append(out, cs)
	}
	if err := rows.Err(); err != nil {
		return out, err
	}
	if err := rows.Close(); err != nil {
		return out, err
	}
	for i := range out {
		if err := s.loadChatSessionDetails(username, &out[i]); err != nil {
			return out, err
		}
	}
	return out, nil
}

// handleChatSessionsList (GET /api/chat/sessions) returns the authenticated
// environment's persisted sessions so the SPA can restore every open tab after a
// reload or a crash. Shape is a bare JSON array of session objects, newest
// activity first. (Distinct from /api/chat/threads, which is the in-tab
// conversation-branch surface.)
func (s *server) handleChatSessionsList(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	sessions, err := s.listChatSessions(currentUser(r))
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to list sessions")
		return
	}
	_ = json.NewEncoder(w).Encode(sessions)
}

// handleChatSession is the per-session CRUD surface:
//
//	GET    /api/chat/session?sid=<id>  → read one session's metadata (back-compat
//	                                      fields session_id/csrf/prompt_tier are
//	                                      preserved for the existing SPA bootstrap)
//	POST   /api/chat/session           → create/rename a session (body: {id|sid,
//	                                      title?, cwd?}); an explicit title is
//	                                      always applied (rename)
//	DELETE /api/chat/session?sid=<id>  → forget a session (close a tab)
//
// All operations require an authenticated actor but address shared state.
func (s *server) handleChatSession(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		s.handleChatSessionGet(w, r)
	case http.MethodPost:
		s.handleChatSessionUpsert(w, r)
	case http.MethodDelete:
		s.handleChatSessionDelete(w, r)
	default:
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
	}
}

func (s *server) handleChatSessionGet(w http.ResponseWriter, r *http.Request) {
	sid := r.URL.Query().Get("sid")
	w.Header().Set("Content-Type", "application/json")

	out := map[string]any{
		"session_id":  sid,
		"csrf":        "",
		"prompt_tier": "standard",
		"exists":      false,
	}
	if cs, err := s.getChatSession(currentUser(r), sid); err == nil && cs != nil {
		out["exists"] = true
		out["title"] = cs.Title
		out["cwd"] = cs.Cwd
		out["provider_session_id"] = cs.ProviderSessionID
		out["messages"] = cs.Messages
		out["created_at"] = cs.CreatedAt
		out["last_active"] = cs.LastActive
	}
	_ = json.NewEncoder(w).Encode(out)
}

func (s *server) handleChatSessionUpsert(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	var req struct {
		ID             string         `json:"id"`
		Sid            string         `json:"sid"`
		AimeeSessionID string         `json:"aimee_session_id"`
		Title          string         `json:"title"`
		Cwd            string         `json:"cwd"`
		LegacyAliasID  string         `json:"legacy_provider_alias_id"`
		Messages       *[]chatMessage `json:"messages"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "bad request")
		return
	}
	id := firstNonEmpty(req.ID, req.Sid, req.AimeeSessionID)
	if id == "" {
		writeJSONError(w, http.StatusBadRequest, "session id required")
		return
	}
	username := currentUser(r)
	if username == "" {
		writeJSONError(w, http.StatusUnauthorized, "authentication required")
		return
	}
	owned, exists, err := s.chatSessionOwned(username, id)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to verify session")
		return
	}
	_ = exists

	now := time.Now().UTC().Format(time.RFC3339)
	// Explicit upsert: unlike the send-path touch, an explicitly supplied title
	// overwrites (this is the rename path). An empty title leaves any existing
	// title untouched.
	aliasID := strings.TrimSpace(req.LegacyAliasID)
	if aliasID != "" {
		transferred, err := s.upsertChatSessionWithLegacyRuntime(username, id, aliasID,
			strings.TrimSpace(req.Title), req.Cwd, now)
		if err != nil {
			writeJSONError(w, http.StatusInternalServerError, "failed to migrate session runtime")
			return
		}
		if !transferred {
			writeJSONError(w, http.StatusConflict, "legacy session alias is unavailable")
			return
		}
	} else if _, err := s.db.Exec(`
			INSERT INTO chat_sessions (id, username, title, cwd, created_at, last_active)
			VALUES (?, ?, ?, ?, ?, ?)
			ON CONFLICT(id) DO UPDATE SET
				last_active = excluded.last_active,
				cwd   = CASE WHEN excluded.cwd   != '' THEN excluded.cwd   ELSE chat_sessions.cwd   END,
				title = CASE WHEN excluded.title != '' THEN excluded.title ELSE chat_sessions.title END`,
		id, username, strings.TrimSpace(req.Title), req.Cwd, now, now); err != nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to save session")
		return
	}
	owned, _, err = s.chatSessionOwned(username, id)
	if err != nil || !owned {
		writeJSONError(w, http.StatusConflict, "session id is unavailable")
		return
	}
	if req.Messages != nil {
		if err := s.seedChatMessages(username, id, *req.Messages); err != nil {
			writeJSONError(w, http.StatusBadRequest, err.Error())
			return
		}
	}

	cs, err := s.getChatSession(username, id)
	if err != nil || cs == nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to load session")
		return
	}
	_ = json.NewEncoder(w).Encode(cs)
}

func (s *server) handleChatSessionDelete(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	sid := r.URL.Query().Get("sid")
	if sid == "" {
		writeJSONError(w, http.StatusBadRequest, "sid required")
		return
	}
	if s.db != nil {
		tx, err := s.db.Begin()
		if err != nil {
			writeJSONError(w, http.StatusInternalServerError, "failed to delete session")
			return
		}
		defer tx.Rollback()
		if _, err = tx.Exec(`DELETE FROM chat_session_messages WHERE session_id = ?`, sid); err == nil {
			_, err = tx.Exec(`DELETE FROM chat_session_runtime WHERE session_id = ?`, sid)
		}
		if err == nil {
			_, err = tx.Exec(`DELETE FROM chat_sessions WHERE id = ?`, sid)
		}
		if err != nil || tx.Commit() != nil {
			writeJSONError(w, http.StatusInternalServerError, "failed to delete session")
			return
		}
	}
	_, _ = w.Write([]byte(`{"status":"ok","deleted":true}`))
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if v != "" {
			return v
		}
	}
	return ""
}

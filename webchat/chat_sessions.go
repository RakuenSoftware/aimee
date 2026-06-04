package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

// Per-user chat-session persistence. A "session" here is one browser tab's
// conversation: identified by the aimee-server conversation id (the same value
// the SPA sends as aimee_session_id and aimee-server echoes back in the
// "session" stream event). The conversation history itself lives on
// aimee-server; this table is webchat's durable record of WHICH sessions belong
// to WHICH logged-in user, so a user can reopen their browser after a crash and
// restore every tab they had open. Rows are scoped by the authenticated
// username so users only ever see and restore their own tabs.

type ctxKey string

const ctxUsername ctxKey = "webchat_username"

// currentUser returns the authenticated username injected by requireAuth, or ""
// if the request was not authenticated (which the handlers treat as no session
// ownership rather than an error).
func currentUser(r *http.Request) string {
	u, _ := r.Context().Value(ctxUsername).(string)
	return u
}

// withUser returns a shallow copy of r carrying the resolved username, used by
// requireAuth so downstream handlers can scope persistence to the user.
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
}

func applyChatSessionMigrations(db *sql.DB) error {
	for _, m := range chatSessionMigrations {
		if _, err := db.Exec(m); err != nil {
			return err
		}
	}
	return nil
}

// chatSession is the durable record of one tab's session for a user.
type chatSession struct {
	ID         string `json:"id"`
	Title      string `json:"title"`
	Cwd        string `json:"cwd,omitempty"`
	CreatedAt  string `json:"created_at"`
	LastActive string `json:"last_active"`
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

// touchChatSession records (or refreshes) a user's session from the chat-send
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
			title = CASE WHEN chat_sessions.title = ''  THEN excluded.title ELSE chat_sessions.title END
		WHERE chat_sessions.username = excluded.username`,
		id, username, title, cwd, now, now)
	return err
}

// getChatSession loads one session owned by the user, or (nil, nil) if absent.
func (s *server) getChatSession(username, id string) (*chatSession, error) {
	if s == nil || s.db == nil || username == "" || id == "" {
		return nil, nil
	}
	row := s.db.QueryRow(
		`SELECT id, title, cwd, created_at, last_active
		   FROM chat_sessions WHERE id = ? AND username = ?`, id, username)
	var cs chatSession
	switch err := row.Scan(&cs.ID, &cs.Title, &cs.Cwd, &cs.CreatedAt, &cs.LastActive); err {
	case nil:
		return &cs, nil
	case sql.ErrNoRows:
		return nil, nil
	default:
		return nil, err
	}
}

// listChatSessions returns the user's sessions, most-recently-active first.
func (s *server) listChatSessions(username string) ([]chatSession, error) {
	out := []chatSession{}
	if s == nil || s.db == nil || username == "" {
		return out, nil
	}
	rows, err := s.db.Query(
		`SELECT id, title, cwd, created_at, last_active
		   FROM chat_sessions WHERE username = ?
		  ORDER BY last_active DESC LIMIT 200`, username)
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
	return out, rows.Err()
}

// handleChatThreads (GET /api/chat/threads) returns the authenticated user's
// persisted sessions so the SPA can restore every open tab after a reload or a
// crash. Shape is a bare JSON array (matching the prior stub) of session
// objects, newest activity first.
func (s *server) handleChatThreads(w http.ResponseWriter, r *http.Request) {
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
// All operations are scoped to the authenticated user.
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
		out["created_at"] = cs.CreatedAt
		out["last_active"] = cs.LastActive
	}
	_ = json.NewEncoder(w).Encode(out)
}

func (s *server) handleChatSessionUpsert(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	var req struct {
		ID             string `json:"id"`
		Sid            string `json:"sid"`
		AimeeSessionID string `json:"aimee_session_id"`
		Title          string `json:"title"`
		Cwd            string `json:"cwd"`
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

	now := time.Now().UTC().Format(time.RFC3339)
	// Explicit upsert: unlike the send-path touch, an explicitly supplied title
	// overwrites (this is the rename path). An empty title leaves any existing
	// title untouched.
	if _, err := s.db.Exec(`
		INSERT INTO chat_sessions (id, username, title, cwd, created_at, last_active)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			last_active = excluded.last_active,
			cwd   = CASE WHEN excluded.cwd   != '' THEN excluded.cwd   ELSE chat_sessions.cwd   END,
			title = CASE WHEN excluded.title != '' THEN excluded.title ELSE chat_sessions.title END
		WHERE chat_sessions.username = excluded.username`,
		id, username, strings.TrimSpace(req.Title), req.Cwd, now, now); err != nil {
		writeJSONError(w, http.StatusInternalServerError, "failed to save session")
		return
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
		if _, err := s.db.Exec(`DELETE FROM chat_sessions WHERE id = ? AND username = ?`,
			sid, currentUser(r)); err != nil {
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

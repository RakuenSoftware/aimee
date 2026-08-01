package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	_ "github.com/mattn/go-sqlite3"
)

// newSessionTestServer returns a server backed by an in-memory SQLite DB with
// the chat-session schema applied.
func newSessionTestServer(t *testing.T) *server {
	t.Helper()
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	if err := applyChatSessionMigrations(db); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return &server{db: db}
}

// asUser wraps a request with an authenticated username, the way requireAuth
// would after validating the session cookie.
func asUser(r *http.Request, username string) *http.Request {
	return withUser(r, username)
}

func TestApplyChatSessionMigrationsPromotesOnlyPreexistingLegacyRows(t *testing.T) {
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	db.SetMaxOpenConns(1)
	defer db.Close()
	if _, err := db.Exec(`CREATE TABLE chat_sessions (
		id TEXT PRIMARY KEY, username TEXT NOT NULL, title TEXT NOT NULL DEFAULT '',
		cwd TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, last_active TEXT NOT NULL
	)`); err != nil {
		t.Fatalf("old schema: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO chat_sessions
		(id, username, created_at, last_active) VALUES
		('legacy-provider-id', 'alice', '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')`); err != nil {
		t.Fatalf("legacy row: %v", err)
	}
	if err := applyChatSessionMigrations(db); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	s := &server{db: db}
	if got, err := s.chatSessionProviderID("alice", "legacy-provider-id"); err != nil || got != "legacy-provider-id" {
		t.Fatalf("legacy runtime = %q, err=%v", got, err)
	}

	if err := s.touchChatSession("alice", "web-stable-id", "", "new chat"); err != nil {
		t.Fatalf("new session: %v", err)
	}
	if err := applyChatSessionMigrations(db); err != nil {
		t.Fatalf("repeat migrate: %v", err)
	}
	if got, err := s.chatSessionProviderID("alice", "web-stable-id"); err != nil || got != "" {
		t.Fatalf("new stable id was misclassified as provider id: %q, err=%v", got, err)
	}
}

func TestTouchAndListChatSessions(t *testing.T) {
	s := newSessionTestServer(t)

	if err := s.touchChatSession("alice", "sess-1", "/proj/a", "first message line\nsecond"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	if err := s.touchChatSession("alice", "sess-2", "/proj/b", "another conversation"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	// Bob's session must not appear in Alice's list.
	if err := s.touchChatSession("bob", "sess-3", "/proj/c", "bob's chat"); err != nil {
		t.Fatalf("touch: %v", err)
	}

	got, err := s.listChatSessions("alice")
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("expected 2 sessions for alice, got %d: %+v", len(got), got)
	}
	// Title derived from first line of the first message only.
	var s1 *chatSession
	for i := range got {
		if got[i].ID == "sess-1" {
			s1 = &got[i]
		}
	}
	if s1 == nil {
		t.Fatal("sess-1 missing from alice's list")
	}
	if s1.Title != "first message line" {
		t.Fatalf("title = %q, want %q", s1.Title, "first message line")
	}
	if s1.Cwd != "/proj/a" {
		t.Fatalf("cwd = %q", s1.Cwd)
	}
}

func TestTouchChatSessionPreservesTitleAcrossTurns(t *testing.T) {
	s := newSessionTestServer(t)
	if err := s.touchChatSession("alice", "sess-1", "/proj/a", "original title"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	// A later turn must not overwrite the established title.
	if err := s.touchChatSession("alice", "sess-1", "/proj/a", "a much later message"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	cs, err := s.getChatSession("alice", "sess-1")
	if err != nil || cs == nil {
		t.Fatalf("get: %v %+v", err, cs)
	}
	if cs.Title != "original title" {
		t.Fatalf("title changed across turns: %q", cs.Title)
	}
}

func TestTouchChatSessionScopedByUser(t *testing.T) {
	s := newSessionTestServer(t)
	if err := s.touchChatSession("alice", "shared-id", "/a", "alice msg"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	// A second user touching the same id must not hijack or mutate alice's row.
	if err := s.touchChatSession("bob", "shared-id", "/b", "bob msg"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	cs, err := s.getChatSession("alice", "shared-id")
	if err != nil || cs == nil {
		t.Fatalf("alice get: %v %+v", err, cs)
	}
	if cs.Cwd != "/a" {
		t.Fatalf("alice row mutated by bob: cwd=%q", cs.Cwd)
	}
	// Bob owns nothing under that id.
	if bobCS, _ := s.getChatSession("bob", "shared-id"); bobCS != nil {
		t.Fatalf("bob unexpectedly owns shared-id: %+v", bobCS)
	}
}

func TestChatSessionRestoresTranscriptAndProviderID(t *testing.T) {
	s := newSessionTestServer(t)
	if err := s.touchChatSession("alice", "stable-web-id", "/proj", "hello"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	if err := s.setChatSessionRuntime("alice", "stable-web-id", "provider-thread-7"); err != nil {
		t.Fatalf("runtime: %v", err)
	}
	if err := s.appendChatMessage("alice", "stable-web-id", "user", "hello"); err != nil {
		t.Fatalf("append user: %v", err)
	}
	if err := s.appendChatMessage("alice", "stable-web-id", "assistant", "hi back"); err != nil {
		t.Fatalf("append assistant: %v", err)
	}

	cs, err := s.getChatSession("alice", "stable-web-id")
	if err != nil || cs == nil {
		t.Fatalf("get: %v %+v", err, cs)
	}
	if cs.ProviderSessionID != "provider-thread-7" {
		t.Fatalf("provider session = %q", cs.ProviderSessionID)
	}
	if len(cs.Messages) != 2 || cs.Messages[0].Role != "user" || cs.Messages[0].Text != "hello" ||
		cs.Messages[1].Role != "assistant" || cs.Messages[1].Text != "hi back" {
		t.Fatalf("messages = %+v", cs.Messages)
	}
	if bob, err := s.getChatSession("bob", "stable-web-id"); err != nil || bob != nil {
		t.Fatalf("bob read alice session: err=%v session=%+v", err, bob)
	}
}

func TestSeedChatMessagesDoesNotOverwriteServerHistory(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "", "server turn")
	if err := s.seedChatMessages("alice", "sess-1", []chatMessage{
		{Role: "user", Text: "legacy user"},
		{Role: "assistant", Text: "legacy answer"},
	}); err != nil {
		t.Fatalf("initial seed: %v", err)
	}
	if err := s.seedChatMessages("alice", "sess-1", []chatMessage{
		{Role: "user", Text: "stale overwrite"},
	}); err != nil {
		t.Fatalf("repeat seed: %v", err)
	}
	cs, _ := s.getChatSession("alice", "sess-1")
	if cs == nil || len(cs.Messages) != 2 || cs.Messages[0].Text != "legacy user" {
		t.Fatalf("server transcript overwritten: %+v", cs)
	}
}

func TestHandleChatSessionsListReturnsUserSessions(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "/a", "hello there")
	_ = s.touchChatSession("bob", "sess-2", "/b", "bob only")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodGet, "/api/chat/sessions", nil), "alice")
	s.handleChatSessionsList(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	var sessions []chatSession
	if err := json.Unmarshal(rr.Body.Bytes(), &sessions); err != nil {
		t.Fatalf("decode: %v (%s)", err, rr.Body.String())
	}
	if len(sessions) != 1 || sessions[0].ID != "sess-1" {
		t.Fatalf("expected only alice's sess-1, got %+v", sessions)
	}
}

func TestHandleChatSessionsListEmptyIsArray(t *testing.T) {
	s := newSessionTestServer(t)
	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodGet, "/api/chat/sessions", nil), "nobody")
	s.handleChatSessionsList(rr, req)
	if got := strings.TrimSpace(rr.Body.String()); got != "[]" {
		t.Fatalf("empty sessions = %q, want []", got)
	}
}

func TestHandleChatSessionUpsertAndGet(t *testing.T) {
	s := newSessionTestServer(t)

	// Create via POST.
	rr := httptest.NewRecorder()
	body := `{"id":"sess-1","cwd":"/proj","title":"My Tab"}`
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(body)), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("upsert status = %d (%s)", rr.Code, rr.Body.String())
	}
	var cs chatSession
	if err := json.Unmarshal(rr.Body.Bytes(), &cs); err != nil {
		t.Fatalf("decode upsert: %v", err)
	}
	if cs.ID != "sess-1" || cs.Title != "My Tab" || cs.Cwd != "/proj" {
		t.Fatalf("unexpected upsert result: %+v", cs)
	}

	// Read via GET — carries back-compat fields + exists=true.
	rr = httptest.NewRecorder()
	req = asUser(httptest.NewRequest(http.MethodGet, "/api/chat/session?sid=sess-1", nil), "alice")
	s.handleChatSession(rr, req)
	var out map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode get: %v", err)
	}
	if out["exists"] != true || out["title"] != "My Tab" || out["prompt_tier"] != "standard" {
		t.Fatalf("unexpected get result: %+v", out)
	}
}

func TestHandleChatSessionUpsertSeedsCrossBrowserStateWithoutTrustingProviderID(t *testing.T) {
	s := newSessionTestServer(t)
	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(
		`{"id":"stable-id","title":"My chat","provider_session_id":"provider-id",`+
			`"messages":[{"role":"user","text":"question"},{"role":"assistant","text":"answer"}]}`)), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", rr.Code, rr.Body.String())
	}
	var got chatSession
	if err := json.Unmarshal(rr.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.ID != "stable-id" || got.ProviderSessionID != "" || len(got.Messages) != 2 {
		t.Fatalf("restorable session = %+v", got)
	}
}

func TestHandleChatSessionUpsertTransfersServerTrustedLegacyAlias(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "legacy-provider-id", "", "old chat")
	_ = s.setChatSessionRuntime("alice", "legacy-provider-id", "legacy-provider-id")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(
		`{"id":"web-stable-id","legacy_provider_alias_id":"legacy-provider-id",`+
			`"messages":[{"role":"user","text":"old chat"}]}`)), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", rr.Code, rr.Body.String())
	}
	cs, err := s.getChatSession("alice", "web-stable-id")
	if err != nil || cs == nil || cs.ProviderSessionID != "legacy-provider-id" || len(cs.Messages) != 1 {
		t.Fatalf("transferred session: err=%v session=%+v", err, cs)
	}
}

func TestHandleChatSessionUpsertRejectsUntrustedLegacyAlias(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "unbound-alias", "", "old chat")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(
		`{"id":"web-stable-id","legacy_provider_alias_id":"unbound-alias"}`)), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409 (%s)", rr.Code, rr.Body.String())
	}
	if got, err := s.chatSessionProviderID("alice", "web-stable-id"); err != nil || got != "" {
		t.Fatalf("untrusted alias established runtime %q, err=%v", got, err)
	}
	if cs, err := s.getChatSession("alice", "web-stable-id"); err != nil || cs != nil {
		t.Fatalf("rejected alias left a target row: err=%v session=%+v", err, cs)
	}
}

func TestHandleChatSessionUpsertRejectsAnotherUsersLegacyAlias(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "alice-provider-id", "", "alice chat")
	_ = s.setChatSessionRuntime("alice", "alice-provider-id", "alice-provider-id")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(
		`{"id":"bob-stable-id","legacy_provider_alias_id":"alice-provider-id"}`)), "bob")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409 (%s)", rr.Code, rr.Body.String())
	}
	if got, err := s.chatSessionProviderID("bob", "bob-stable-id"); err != nil || got != "" {
		t.Fatalf("cross-user alias established runtime %q, err=%v", got, err)
	}
	if cs, err := s.getChatSession("bob", "bob-stable-id"); err != nil || cs != nil {
		t.Fatalf("cross-user alias left a target row: err=%v session=%+v", err, cs)
	}
}

func TestHandleChatSessionUpsertRejectsAnotherUsersID(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "shared-id", "", "alice")
	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session",
		strings.NewReader(`{"id":"shared-id","title":"hijacked"}`)), "bob")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409 (%s)", rr.Code, rr.Body.String())
	}
	cs, _ := s.getChatSession("alice", "shared-id")
	if cs == nil || cs.Title == "hijacked" {
		t.Fatalf("alice session mutated: %+v", cs)
	}
}

func TestHandleChatSessionRenameOverwritesTitle(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "/a", "auto derived title")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/session",
		strings.NewReader(`{"id":"sess-1","title":"Renamed"}`)), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	cs, _ := s.getChatSession("alice", "sess-1")
	if cs == nil || cs.Title != "Renamed" {
		t.Fatalf("rename failed: %+v", cs)
	}
}

func TestHandleChatSessionDelete(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "/a", "to be deleted")
	_ = s.setChatSessionRuntime("alice", "sess-1", "provider-1")
	_ = s.appendChatMessage("alice", "sess-1", "user", "to be deleted")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodDelete, "/api/chat/session?sid=sess-1", nil), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("delete status = %d", rr.Code)
	}
	if cs, _ := s.getChatSession("alice", "sess-1"); cs != nil {
		t.Fatalf("session not deleted: %+v", cs)
	}
	var messages, runtime int
	_ = s.db.QueryRow(`SELECT COUNT(*) FROM chat_session_messages WHERE session_id = 'sess-1'`).Scan(&messages)
	_ = s.db.QueryRow(`SELECT COUNT(*) FROM chat_session_runtime WHERE session_id = 'sess-1'`).Scan(&runtime)
	if messages != 0 || runtime != 0 {
		t.Fatalf("child state remains: messages=%d runtime=%d", messages, runtime)
	}
}

func TestHandleChatSessionDeleteScopedByUser(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "/a", "alice's")

	// Bob attempts to delete Alice's session — must not succeed.
	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodDelete, "/api/chat/session?sid=sess-1", nil), "bob")
	s.handleChatSession(rr, req)
	if cs, _ := s.getChatSession("alice", "sess-1"); cs == nil {
		t.Fatal("bob deleted alice's session")
	}
}

func TestHandleChatSessionUpsertRequiresAuth(t *testing.T) {
	s := newSessionTestServer(t)
	rr := httptest.NewRecorder()
	// No user in context (unauthenticated).
	req := httptest.NewRequest(http.MethodPost, "/api/chat/session", strings.NewReader(`{"id":"x"}`))
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rr.Code)
	}
}

func TestCurrentUserDefaultsEmpty(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	if u := currentUser(req); u != "" {
		t.Fatalf("currentUser = %q, want empty", u)
	}
	req = req.WithContext(context.WithValue(req.Context(), ctxUsername, "zoe"))
	if u := currentUser(req); u != "zoe" {
		t.Fatalf("currentUser = %q, want zoe", u)
	}
}

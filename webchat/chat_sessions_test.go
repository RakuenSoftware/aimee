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

func TestHandleChatThreadsReturnsUserSessions(t *testing.T) {
	s := newSessionTestServer(t)
	_ = s.touchChatSession("alice", "sess-1", "/a", "hello there")
	_ = s.touchChatSession("bob", "sess-2", "/b", "bob only")

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodGet, "/api/chat/threads", nil), "alice")
	s.handleChatThreads(rr, req)

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

func TestHandleChatThreadsEmptyIsArray(t *testing.T) {
	s := newSessionTestServer(t)
	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodGet, "/api/chat/threads", nil), "nobody")
	s.handleChatThreads(rr, req)
	if got := strings.TrimSpace(rr.Body.String()); got != "[]" {
		t.Fatalf("empty threads = %q, want []", got)
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

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodDelete, "/api/chat/session?sid=sess-1", nil), "alice")
	s.handleChatSession(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("delete status = %d", rr.Code)
	}
	if cs, _ := s.getChatSession("alice", "sess-1"); cs != nil {
		t.Fatalf("session not deleted: %+v", cs)
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

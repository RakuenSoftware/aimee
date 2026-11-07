package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"github.com/RakuenSoftware/smoothgui/auth"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

// An explicitly synthetic authenticated browser session exercises the actual
// signed-cookie validator and real Go HTTP handler against the owned real
// native HTTP/PG/Vault/module pipeline. It does not test PAM or browser login.
func TestOwnedOriginalInputNativeHTTP(t *testing.T) {
	socket := os.Getenv("AIMS_OWNED_NATIVE_SOCKET")
	if socket == "" {
		t.Skip("requires runner-owned native fixture; never targets installed services")
	}
	canary := os.Getenv("AIMS_OWNED_RAW_CANARY")
	if canary == "" {
		t.Fatal("owned canary missing")
	}
	s := newSessionTestServer(t)
	for _, migration := range auth.Migrations {
		if _, err := s.db.Exec(migration); err != nil {
			t.Fatal(err)
		}
	}
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		t.Fatal(err)
	}
	vault := &fakeWebchatVault{snapshot: webchatVaultSnapshot{SessionHMAC: hex.EncodeToString(key)}}
	sessions, err := newSignedSessionStore(s.db, vault, time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	s.sessions = sessions
	s.cfg = &config{socketPath: socket, requireOriginalInput: true}
	s.originalInputMode = true
	token, err := sessions.CreateSession("memory-fixture")
	if err != nil {
		t.Fatal(err)
	}
	sid := "web-74000000-0000-4000-8000-000000000001"
	operation := "74000000-0000-4000-8000-000000000002"
	body := map[string]any{"aimee_session_id": sid, "message": "password: " + canary, "original_input_operation_uuid": operation}
	invoke := func(cookie string, body map[string]any) *httptest.ResponseRecorder {
		encoded, err := json.Marshal(body)
		if err != nil {
			t.Fatal(err)
		}
		request := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(string(encoded)))
		request.Header.Set("X-Aimee-Webuser", "forged-browser-header")
		request.AddCookie(&http.Cookie{Name: "session", Value: cookie})
		response := httptest.NewRecorder()
		s.requireAuth(s.handleChatSend)(response, request)
		return response
	}
	// HMAC failure is rejected before any native forwarding or chat metadata.
	bad := invoke(token+"0", body)
	if bad.Code != http.StatusUnauthorized {
		t.Fatal("invalid signed cookie accepted", bad.Code)
	}
	missing := map[string]any{"aimee_session_id": sid, "message": "password: " + canary}
	invalid := invoke(token, missing)
	if invalid.Code != http.StatusBadRequest {
		t.Fatal("missing action accepted", invalid.Code, invalid.Body.String())
	}
	var rows int
	if err = s.db.QueryRow("SELECT COUNT(*) FROM chat_sessions").Scan(&rows); err != nil || rows != 0 {
		t.Fatal("invalid request persisted metadata", rows, err)
	}
	response := invoke(token, body)
	if response.Code != http.StatusOK || strings.Contains(response.Body.String(), canary) || !strings.Contains(response.Body.String(), "Bounded normalized reply.") {
		t.Fatal("owned native response failed", response.Code, response.Body.String())
	}
	cs, err := s.getChatSession("memory-fixture", sid)
	if err != nil || cs == nil {
		t.Fatal(err)
	}
	if cs.Title == "" || strings.Contains(cs.Title, canary) || !strings.Contains(cs.Title, "[[aims-protected:") {
		t.Fatal("title did not come from normalized committed User", cs.Title)
	}
	if expected := os.Getenv("AIMS_OWNED_EXPECT_CWD"); expected == "" || cs.Cwd != expected {
		t.Fatal("committed cwd mismatch", cs.Cwd, expected)
	}
	if len(cs.Messages) != 0 {
		t.Fatal("raw/noncanonical transcript persisted in Go")
	}
	replay := invoke(token, body)
	if strings.Contains(replay.Body.String(), canary) {
		t.Fatal("raw replay response")
	}
	after, err := s.getChatSession("memory-fixture", sid)
	if err != nil || after == nil || after.Title != cs.Title || after.Cwd != cs.Cwd {
		t.Fatal("replay changed committed metadata", err)
	}
	proof := map[string]any{"passed": true, "actual_go_http_handler": true, "actual_signed_cookie_validation": true, "synthetic_login_fixture": true, "actual_pam_login": false, "tampered_cookie_refused": true, "missing_operation_refused_before_metadata": true, "native_required_negotiation": s.originalInputContract.Load() == 2, "canonical_normalized_title": true, "raw_canary_absent_from_response_and_chat_metadata": true, "session": sid, "operation_uuid": operation}
	encoded, err := json.MarshalIndent(proof, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(os.Getenv("AIMS_OWNED_GO_PROOF"), append(encoded, '\n'), 0600); err != nil {
		t.Fatal(err)
	}
}

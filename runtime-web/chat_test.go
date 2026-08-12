package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
	_ "github.com/mattn/go-sqlite3"
)

type observingResponseRecorder struct {
	*httptest.ResponseRecorder
	onWrite func([]byte)
}

func addWebSession(t *testing.T, s *server, req *http.Request, username string) {
	t.Helper()
	if s.sessions == nil {
		db, err := sql.Open("sqlite3", filepath.Join(t.TempDir(), "sessions.db"))
		if err != nil {
			t.Fatalf("open session db: %v", err)
		}
		for _, migration := range auth.Migrations {
			if _, err := db.Exec(migration); err != nil {
				db.Close()
				t.Fatalf("auth migration: %v", err)
			}
		}
		s.sessions = auth.NewSessionStore(db, time.Hour)
		t.Cleanup(func() { db.Close() })
	}
	token, err := s.sessions.CreateSession(username)
	if err != nil {
		t.Fatalf("create web session: %v", err)
	}
	req.AddCookie(&http.Cookie{Name: "session", Value: token})
}

func (r *observingResponseRecorder) Write(p []byte) (int, error) {
	if r.onWrite != nil {
		r.onWrite(p)
	}
	return r.ResponseRecorder.Write(p)
}

func TestHandleChatInitRulesCreatesRulesFile(t *testing.T) {
	tmp := t.TempDir()

	s := &server{}
	resp := postInitRules(t, s, tmp)
	if !resp.OK || resp.Status != "ok" || !resp.Created || resp.AlreadyExists {
		t.Fatalf("unexpected first response: %+v", resp)
	}

	raw, err := os.ReadFile(filepath.Join(tmp, ".aimee-rules"))
	if err != nil {
		t.Fatalf("read generated rules: %v", err)
	}
	if !strings.Contains(string(raw), "# Aimee Rules") {
		t.Fatalf("generated rules missing header: %q", string(raw))
	}

	resp = postInitRules(t, s, tmp)
	if !resp.OK || resp.Status != "ok" || resp.Created || !resp.AlreadyExists {
		t.Fatalf("unexpected second response: %+v", resp)
	}
}

func TestHandleBootstrapStatusUsesSelectedProject(t *testing.T) {
	withRules := t.TempDir()
	withoutRules := t.TempDir()
	if err := os.WriteFile(filepath.Join(withRules, ".aimee-rules"), []byte("# rules\n"), 0644); err != nil {
		t.Fatalf("write rules: %v", err)
	}

	s := &server{}
	if !bootstrapHasRules(t, s, withRules) {
		t.Fatal("expected selected project with .aimee-rules to report has_rules")
	}
	if bootstrapHasRules(t, s, withoutRules) {
		t.Fatal("expected selected project without .aimee-rules to report missing rules")
	}
}

func TestHandleChatProjectsIncludesCurrentRoot(t *testing.T) {
	tmp := t.TempDir()
	oldwd, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	if err := os.Chdir(tmp); err != nil {
		t.Fatalf("chdir temp dir: %v", err)
	}
	defer func() {
		if err := os.Chdir(oldwd); err != nil {
			t.Fatalf("restore cwd: %v", err)
		}
	}()

	s := &server{}
	req := httptest.NewRequest(http.MethodGet, "/api/chat/projects", nil)
	rr := httptest.NewRecorder()
	s.handleChatProjects(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	var resp struct {
		CurrentRoot string        `json:"current_root"`
		Projects    []chatProject `json:"projects"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.CurrentRoot != tmp {
		t.Fatalf("current_root = %q, want %q", resp.CurrentRoot, tmp)
	}
	if len(resp.Projects) != 1 || resp.Projects[0].Root != tmp || !resp.Projects[0].Current {
		t.Fatalf("unexpected projects response: %+v", resp.Projects)
	}
}

func TestHandleOpenAIModelsRequiresWebSession(t *testing.T) {
	tmp := t.TempDir()

	s := &server{cfg: &config{socketPath: filepath.Join(tmp, "aimee.sock")}}
	req := httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	rr := httptest.NewRecorder()
	s.handleOpenAIModels(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401; body = %s", rr.Code, rr.Body.String())
	}

	req = httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	req.Header.Set("Authorization", "Bearer wrong-token")
	rr = httptest.NewRecorder()
	s.handleOpenAIModels(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401; body = %s", rr.Code, rr.Body.String())
	}
}

func TestHandleOpenAIModelsReturnsOpenAICompatibleList(t *testing.T) {
	tmp := t.TempDir()
	s := &server{cfg: &config{socketPath: filepath.Join(tmp, "aimee.sock")}}
	req := httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	addWebSession(t, s, req, "alice")
	rr := httptest.NewRecorder()
	s.handleOpenAIModels(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}

	var resp struct {
		Object string `json:"object"`
		Data   []struct {
			ID      string `json:"id"`
			Object  string `json:"object"`
			OwnedBy string `json:"owned_by"`
		} `json:"data"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Object != "list" || len(resp.Data) != 1 {
		t.Fatalf("unexpected response: %+v", resp)
	}
	if resp.Data[0].ID != "aimee" || resp.Data[0].Object != "model" || resp.Data[0].OwnedBy != "aimee" {
		t.Fatalf("unexpected model entry: %+v", resp.Data[0])
	}
}

// TestRPCV1CallDispatchesOverV1 proves the unary RPC path (socketCallForRequest
// → rpcV1Call) resolves a GET-backed method to its first-class /v1 route (no
// body) and decodes the dispatch envelope returned byte-for-byte by the server's
// rh_dispatch_op.
func TestRPCV1CallDispatchesOverV1(t *testing.T) {
	var gotMethod, gotPath string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/model/list", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"status":"ok","agents":[{"name":"aimee"}]}`)
	})
	cfg := startFakeV1(t, mux)
	s := &server{cfg: cfg}

	resp, err := s.rpcV1Call(context.Background(), map[string]any{"method": "model.list"})
	if err != nil {
		t.Fatalf("rpcV1Call: %v", err)
	}
	if gotMethod != http.MethodGet || gotPath != "/v1/model/list" {
		t.Fatalf("dispatched %s %s, want GET /v1/model/list", gotMethod, gotPath)
	}
	if _, ok := resp["agents"]; !ok {
		t.Fatalf("decoded response missing agents: %v", resp)
	}
}

// TestRPCV1CallForwardsBodyForPOSTRoute proves a POST-backed method reaches its
// first-class route and forwards the request body (args) — e.g. mcp.call carries
// its tool/arguments.
func TestRPCV1CallForwardsBodyForPOSTRoute(t *testing.T) {
	var gotMethod, gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/mcp/call", func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		gotMethod, gotPath, gotBody = r.Method, r.URL.Path, string(b)
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"status":"ok","structuredContent":{"sessions":[]}}`)
	})
	cfg := startFakeV1(t, mux)
	s := &server{cfg: cfg}

	resp, err := s.rpcV1Call(context.Background(), map[string]any{
		"method": "mcp.call", "tool": "session_list",
		"arguments": map[string]any{"limit": 100},
	})
	if err != nil {
		t.Fatalf("rpcV1Call: %v", err)
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/mcp/call" {
		t.Fatalf("dispatched %s %s, want POST /v1/mcp/call", gotMethod, gotPath)
	}
	if !strings.Contains(gotBody, `"session_list"`) {
		t.Fatalf("request body missing tool args: %q", gotBody)
	}
	if _, ok := resp["structuredContent"]; !ok {
		t.Fatalf("decoded response missing structuredContent: %v", resp)
	}
}

// TestRPCV1CallSurfacesServerError proves a non-ok dispatch response is turned
// into an error (matching the legacy socket path's rpcError behaviour).
func TestRPCV1CallSurfacesServerError(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/model/list", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"status":"error","message":"nope"}`)
	})
	cfg := startFakeV1(t, mux)
	s := &server{cfg: cfg}

	if _, err := s.rpcV1Call(context.Background(), map[string]any{"method": "model.list"}); err == nil {
		t.Fatal("expected an error from a status:error response")
	}
}

func TestHandleOpenAIModelsProxiesV1Models(t *testing.T) {
	var gotMethod, gotPath string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/models", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		fmt.Fprint(w, `{"object":"list","data":[`+
			`{"id":"aimee","object":"model","owned_by":"aimee"},`+
			`{"id":"minimax","object":"model","owned_by":"aimee"}]}`)
	})
	cfg := startFakeV1(t, mux)
	s := &server{cfg: cfg}

	req := httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	addWebSession(t, s, req, "alice")
	rr := httptest.NewRecorder()
	s.handleOpenAIModels(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodGet || gotPath != "/v1/models" {
		t.Fatalf("proxied %s %s, want GET /v1/models", gotMethod, gotPath)
	}
	if !strings.Contains(rr.Body.String(), `"minimax"`) {
		t.Fatalf("expected the proxied live model list, got %q", rr.Body.String())
	}
}

// openAIChatServer wires a fake /v1/chat/completions endpoint.
func openAIChatServer(t *testing.T, handler http.HandlerFunc) *server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/completions", handler)
	cfg := startFakeV1(t, mux)
	return &server{cfg: cfg}
}

func TestHandleOpenAIChatCompletionsRequiresBearer(t *testing.T) {
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Fatal("upstream must not be reached without a valid bearer")
	})
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions",
		strings.NewReader(`{"messages":[]}`))
	req.Header.Set("Authorization", "Bearer wrong")
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)
	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rr.Code)
	}
}

func TestHandleOpenAIChatCompletionsProxiesV1(t *testing.T) {
	var gotMethod, gotPath, gotBody, gotAuth string
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath, gotAuth = r.Method, r.URL.Path, r.Header.Get("Authorization")
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"id":"chatcmpl-x","object":"chat.completion",`+
			`"choices":[{"message":{"role":"assistant","content":"hi"}}]}`)
	})

	body := `{"model":"aimee","messages":[{"role":"user","content":"hi"}]}`
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions", strings.NewReader(body))
	addWebSession(t, s, req, "alice")
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/chat/completions" {
		t.Fatalf("proxied %s %s, want POST /v1/chat/completions", gotMethod, gotPath)
	}
	if gotAuth != "" {
		t.Fatalf("webchat bearer leaked upstream: %q", gotAuth)
	}
	if !strings.Contains(gotBody, `"content":"hi"`) {
		t.Fatalf("request body not forwarded: %q", gotBody)
	}
	if !strings.Contains(rr.Body.String(), `"object":"chat.completion"`) {
		t.Fatalf("upstream response not proxied back: %q", rr.Body.String())
	}
}

func TestOpenAIProxyStampsTrustedMetadata(t *testing.T) {
	// A validated web session stamps its server-derived identity while every
	// client-supplied identity/auth header is stripped.

	var gotPrincipal, gotSource, gotProxyAuth, gotSession string
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		gotPrincipal = r.Header.Get("X-Aimee-Principal")
		gotSource = r.Header.Get("X-Aimee-Source")
		gotProxyAuth = r.Header.Get("X-Aimee-Proxy-Authorization")
		gotSession = r.Header.Get("X-Aimee-Session-Key")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"id":"x","object":"chat.completion","choices":[]}`)
	})

	body := `{"model":"aimee","user":"bob","messages":[{"role":"user","content":"hi"}]}`
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions", strings.NewReader(body))
	addWebSession(t, s, req, "alice")
	req.Header.Set("X-Aimee-Principal", "victim") // spoof attempt — must be stripped
	req.Header.Set("X-Aimee-Proxy-Authorization", "forged")
	req.Header.Set("X-Aimee-Session-Key", "spoofed-session") // spoof — must be stripped
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	if gotPrincipal != "webchat:alice" {
		t.Fatalf("principal = %q, want trusted session identity", gotPrincipal)
	}
	if gotSource != "webchat" {
		t.Fatalf("source = %q, want \"webchat\"", gotSource)
	}
	if gotProxyAuth != "" {
		t.Fatalf("proxy auth = %q, want empty (UDS peer attestation needs no secret)", gotProxyAuth)
	}
	if gotSession != "webchat:alice" {
		t.Fatalf("session key = %q, want trusted session identity", gotSession)
	}
}

func TestOpenAIProxyTrustedSessionStampsPAMUser(t *testing.T) {
	// A request carrying a VALID webchat session cookie must be attributed to the
	// trusted PAM username — X-Aimee-Principal / X-Aimee-Session-Key = webchat:<user>
	// — so distinct logged-in webchat users get distinct trusted attribution and do
	// not collapse. This is the proposal's "trusted per-client" requirement.
	t.Setenv("AIMEE_INGRESS_PROXY_SECRET", "topsecret")

	var gotPrincipal, gotSession string
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		gotPrincipal = r.Header.Get("X-Aimee-Principal")
		gotSession = r.Header.Get("X-Aimee-Session-Key")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"id":"x","object":"chat.completion","choices":[]}`)
	})

	// Back the server with a real session store and a valid "alice" session.
	// Pin to one connection so the in-memory DB (and the sessions table the store
	// creates on it) is shared across the store's calls.
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	db.SetMaxOpenConns(1)
	defer db.Close()
	for _, m := range auth.Migrations {
		if _, err := db.Exec(m); err != nil {
			t.Fatalf("auth migration: %v", err)
		}
	}
	s.sessions = auth.NewSessionStore(db, time.Hour)
	token, err := s.sessions.CreateSession("alice")
	if err != nil {
		t.Fatalf("create session: %v", err)
	}

	body := `{"model":"aimee","user":"attacker-chosen","messages":[{"role":"user","content":"hi"}]}`
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions", strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer test-token")
	req.AddCookie(&http.Cookie{Name: "session", Value: token})
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	if gotPrincipal != "webchat:alice" {
		t.Fatalf("principal = %q, want trusted \"webchat:alice\" (the PAM session user)", gotPrincipal)
	}
	if gotSession != "webchat:alice" {
		t.Fatalf("session key = %q, want \"webchat:alice\" (not the client `user` field)", gotSession)
	}
}

func TestHandleMeReturnsAuthenticatedUsername(t *testing.T) {
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	db.SetMaxOpenConns(1)
	defer db.Close()
	for _, migration := range auth.Migrations {
		if _, err := db.Exec(migration); err != nil {
			t.Fatalf("auth migration: %v", err)
		}
	}
	store := auth.NewSessionStore(db, time.Hour)
	token, err := store.CreateSession("alice")
	if err != nil {
		t.Fatalf("create session: %v", err)
	}
	s := &server{sessions: store}
	handler := s.requireAuth(s.handleMe)
	req := httptest.NewRequest(http.MethodGet, "/api/auth/me", nil)
	req.AddCookie(&http.Cookie{Name: "session", Value: token})
	rr := httptest.NewRecorder()
	handler.ServeHTTP(rr, req)
	if rr.Code != http.StatusOK || strings.TrimSpace(rr.Body.String()) != `{"username":"alice"}` {
		t.Fatalf("status=%d body=%q", rr.Code, rr.Body.String())
	}
}

func TestOpenAIProxyNeedsNoSharedSecret(t *testing.T) {

	var gotPrincipal, gotProxyAuth string
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		gotPrincipal = r.Header.Get("X-Aimee-Principal")
		gotProxyAuth = r.Header.Get("X-Aimee-Proxy-Authorization")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"id":"x","object":"chat.completion","choices":[]}`)
	})

	body := `{"model":"aimee","messages":[{"role":"user","content":"hi"}]}`
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions", strings.NewReader(body))
	addWebSession(t, s, req, "bob")
	req.Header.Set("X-Aimee-Principal", "victim")
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	if gotPrincipal != "webchat:bob" {
		t.Fatalf("principal = %q, want kernel-attested webchat identity", gotPrincipal)
	}
	if gotProxyAuth != "" {
		t.Fatalf("proxy auth = %q, want empty", gotProxyAuth)
	}
}

func TestHandleOpenAIChatCompletionsStreamsSSE(t *testing.T) {
	s := openAIChatServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		fmt.Fprint(w, "data: {\"choices\":[{\"delta\":{\"content\":\"he\"}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"delta\":{\"content\":\"llo\"}}]}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	})

	body := `{"model":"aimee","stream":true,"messages":[{"role":"user","content":"hi"}]}`
	req := httptest.NewRequest(http.MethodPost, "/v1/chat/completions", strings.NewReader(body))
	addWebSession(t, s, req, "alice")
	rr := httptest.NewRecorder()
	s.handleOpenAIChatCompletions(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); !strings.Contains(ct, "text/event-stream") {
		t.Fatalf("content-type = %q, want SSE", ct)
	}
	out := rr.Body.String()
	if !strings.Contains(out, `"content":"he"`) || !strings.Contains(out, `[DONE]`) {
		t.Fatalf("SSE frames not passed through: %q", out)
	}
}

func TestResolveProjectRootRejectsRelativePath(t *testing.T) {
	if _, err := resolveProjectRoot("relative/path"); err == nil {
		t.Fatal("expected relative project path to fail")
	}
}

func TestChatStreamHTTPReadsNDJSONEvents(t *testing.T) {
	var gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/x-ndjson")
		w.WriteHeader(http.StatusOK)
		for _, line := range []string{
			`{"event":"turn_start"}`,
			`{"event":"text","content":"he"}`,
			`{"event":"text","content":"llo"}`,
			`{"event":"usage","in":1,"out":2,"cost":0}`,
			`{"event":"done"}`,
			`{"status":"ok"}`,
		} {
			fmt.Fprintln(w, line)
		}
	})
	cfg := startFakeV1(t, mux)

	var events []string
	var text string
	err := chatStreamHTTP(context.Background(), cfg.socketPath, "hi there friend", "web1", "", "", "",
		func(evt streamEvent) {
			events = append(events, evt.Event)
			if evt.Event == "text" {
				text += evt.Content
			}
		})
	if err != nil {
		t.Fatalf("chatStreamHTTP: %v", err)
	}
	if gotPath != "/v1/chat/stream" {
		t.Fatalf("proxied path = %q", gotPath)
	}
	if !strings.Contains(gotBody, `"message":"hi there friend"`) ||
		!strings.Contains(gotBody, `"aimee_session_id":"web1"`) {
		t.Fatalf("request body not forwarded: %q", gotBody)
	}
	if text != "hello" {
		t.Fatalf("reassembled text = %q, want hello", text)
	}
	// turn_start, text, text, usage, done — the trailing {status:ok} terminates
	// the stream and is not delivered as an event.
	if len(events) != 5 {
		t.Fatalf("events = %v, want 5", events)
	}
}

func TestHandleChatSendPersistsStableUserSession(t *testing.T) {
	workspace := t.TempDir()
	var forwardedProviderID string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, `{"root":%q}`, workspace)
	})
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]string
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Fatalf("decode stream request: %v", err)
		}
		forwardedProviderID = body["claude_session_id"]
		w.Header().Set("Content-Type", "application/x-ndjson")
		fmt.Fprintln(w, `{"event":"turn_start"}`)
		fmt.Fprintln(w, `{"event":"session","id":"provider-thread"}`)
		fmt.Fprintln(w, `{"event":"text","content":"hello "}`)
		fmt.Fprintln(w, `{"event":"text","content":"again"}`)
		fmt.Fprintln(w, `{"event":"done"}`)
		fmt.Fprintln(w, `{"status":"ok"}`)
	})
	cfg := startFakeV1(t, mux)
	s := newSessionTestServer(t)
	s.cfg = cfg

	runtimeAtSessionEvent := ""
	rr := &observingResponseRecorder{ResponseRecorder: httptest.NewRecorder()}
	rr.onWrite = func(p []byte) {
		if strings.Contains(string(p), "event: session") {
			runtimeAtSessionEvent, _ = s.chatSessionProviderID("alice", "stable-web-id")
		}
	}
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(
		`{"aimee_session_id":"stable-web-id","claude_session_id":"attacker-thread","message":"question"}`)), "alice")
	s.handleChatSend(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", rr.Code, rr.Body.String())
	}

	cs, err := s.getChatSession("alice", "stable-web-id")
	if err != nil || cs == nil {
		t.Fatalf("stable session missing: %v %+v", err, cs)
	}
	if cs.ProviderSessionID != "provider-thread" {
		t.Fatalf("provider session = %q", cs.ProviderSessionID)
	}
	if forwardedProviderID != "" {
		t.Fatalf("new session trusted client provider id %q", forwardedProviderID)
	}
	if runtimeAtSessionEvent != "provider-thread" {
		t.Fatalf("runtime at browser session event = %q", runtimeAtSessionEvent)
	}
	if len(cs.Messages) != 2 || cs.Messages[0].Text != "question" || cs.Messages[1].Text != "hello again" {
		t.Fatalf("messages = %+v", cs.Messages)
	}
	if wrong, _ := s.getChatSession("alice", "provider-thread"); wrong != nil {
		t.Fatalf("provider id incorrectly became the stable key: %+v", wrong)
	}
}

func TestHandleChatSendUsesOnlyServerBoundProviderSession(t *testing.T) {
	workspace := t.TempDir()
	var forwardedProviderID string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, `{"root":%q}`, workspace)
	})
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]string
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Fatalf("decode stream request: %v", err)
		}
		forwardedProviderID = body["claude_session_id"]
		w.Header().Set("Content-Type", "application/x-ndjson")
		fmt.Fprintln(w, `{"event":"done"}`)
		fmt.Fprintln(w, `{"status":"ok"}`)
	})
	cfg := startFakeV1(t, mux)
	s := newSessionTestServer(t)
	s.cfg = cfg
	if err := s.touchChatSession("alice", "stable-web-id", workspace, "first turn"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	if err := s.setChatSessionRuntime("alice", "stable-web-id", "trusted-provider-thread"); err != nil {
		t.Fatalf("runtime: %v", err)
	}

	rr := httptest.NewRecorder()
	req := asUser(httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(
		`{"aimee_session_id":"stable-web-id","claude_session_id":"attacker-thread","message":"continue"}`)), "alice")
	s.handleChatSend(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", rr.Code, rr.Body.String())
	}
	if forwardedProviderID != "trusted-provider-thread" {
		t.Fatalf("forwarded provider id = %q, want server-bound id", forwardedProviderID)
	}
	cs, err := s.getChatSession("alice", "stable-web-id")
	if err != nil || cs == nil || cs.ProviderSessionID != "trusted-provider-thread" {
		t.Fatalf("server runtime binding changed: err=%v session=%+v", err, cs)
	}
}

func TestChatStreamHTTPSurfacesStatusError(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/x-ndjson")
		fmt.Fprintln(w, `{"status":"error","message":"no provider configured"}`)
	})
	cfg := startFakeV1(t, mux)

	err := chatStreamHTTP(context.Background(), cfg.socketPath, "hi", "", "", "", "",
		func(evt streamEvent) {})
	if err == nil || !strings.Contains(err.Error(), "no provider configured") {
		t.Fatalf("expected status:error surfaced, got %v", err)
	}
}

func postInitRules(t *testing.T, s *server, cwd string) struct {
	OK            bool   `json:"ok"`
	Status        string `json:"status"`
	Created       bool   `json:"created"`
	AlreadyExists bool   `json:"already_exists"`
} {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/init-rules", strings.NewReader(`{"cwd":`+quoteJSON(cwd)+`}`))
	rr := httptest.NewRecorder()
	s.handleChatInitRules(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}

	var resp struct {
		OK            bool   `json:"ok"`
		Status        string `json:"status"`
		Created       bool   `json:"created"`
		AlreadyExists bool   `json:"already_exists"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	return resp
}

func bootstrapHasRules(t *testing.T, s *server, cwd string) bool {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, "/api/chat/bootstrap-status?cwd="+url.QueryEscape(cwd), nil)
	rr := httptest.NewRecorder()
	s.handleBootstrapStatus(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rr.Code, rr.Body.String())
	}
	var resp struct {
		HasRules bool `json:"has_rules"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	return resp.HasRules
}

func quoteJSON(value string) string {
	raw, _ := json.Marshal(value)
	return string(raw)
}

// startFakeV1 stands up an HTTP server on <tmp>/aimee-http.sock (where the
// persona proxy looks for aimee-server's /v1 API) and returns the matching cfg.
func startFakeV1(t *testing.T, handler http.Handler) *config {
	t.Helper()
	tmp := t.TempDir()
	ln, err := net.Listen("unix", filepath.Join(tmp, "aimee-http.sock"))
	if err != nil {
		t.Fatalf("listen v1: %v", err)
	}
	srv := &http.Server{Handler: handler}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close(); ln.Close() })
	return &config{socketPath: filepath.Join(tmp, "aimee.sock")}
}

func TestHandleChatPersonasProxiesV1List(t *testing.T) {
	var gotMethod, gotPath string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/personas", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		fmt.Fprint(w, `{"personas":[{"name":"engineer","description":"d","builtin":true}]}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	s.handleChatPersonas(rr, httptest.NewRequest(http.MethodGet, "/api/chat/personas", nil))
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"engineer"`) {
		t.Fatalf("list: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodGet || gotPath != "/v1/personas" {
		t.Fatalf("proxied %s %s", gotMethod, gotPath)
	}
}

func TestHandleChatPersonaSetProxiesV1(t *testing.T) {
	var gotMethod, gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/persona", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		fmt.Fprint(w, `{"name":"novel","delegates":"readonly"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/persona", strings.NewReader(`{"sid":"web1","name":"novel"}`))
	s.handleChatPersona(rr, req)
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"novel"`) {
		t.Fatalf("set: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/sessions/web1/persona" {
		t.Fatalf("proxied %s %s", gotMethod, gotPath)
	}
	var sent map[string]string
	if json.Unmarshal([]byte(gotBody), &sent); sent["name"] != "novel" {
		t.Fatalf("forwarded body = %q", gotBody)
	}
}

func TestHandleChatPersonasDegradesWhenServerDown(t *testing.T) {
	s := &server{cfg: &config{socketPath: filepath.Join(t.TempDir(), "aimee.sock")}}
	rr := httptest.NewRecorder()
	s.handleChatPersonas(rr, httptest.NewRequest(http.MethodGet, "/api/chat/personas", nil))
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"personas":[]`) {
		t.Fatalf("expected empty-list fallback, got code=%d body=%q", rr.Code, rr.Body.String())
	}
}

func TestHandleChatAttachProxiesV1(t *testing.T) {
	var gotMethod, gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/attach", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		fmt.Fprint(w, `{"session_id":"web1","attach_id":"att-7","events_url":"/v1/sessions/web1/events"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/attach", strings.NewReader(`{"sid":"web1"}`))
	s.handleChatAttach(rr, req)
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"att-7"`) {
		t.Fatalf("attach: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/sessions/web1/attach" {
		t.Fatalf("proxied %s %s", gotMethod, gotPath)
	}
	var sent map[string]any
	json.Unmarshal([]byte(gotBody), &sent)
	if sent["surface"] != "webchat" {
		t.Fatalf("forwarded body = %q (want surface=webchat)", gotBody)
	}
}

func TestHandleChatAttachRequiresSid(t *testing.T) {
	s := &server{cfg: &config{}}
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/attach", strings.NewReader(`{}`))
	s.handleChatAttach(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("missing sid: code=%d", rr.Code)
	}
}

func TestHandleChatDetachProxiesV1(t *testing.T) {
	var gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/detach", func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		fmt.Fprint(w, `{"detached":true}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/detach", strings.NewReader(`{"sid":"web1","attach_id":"att-7"}`))
	s.handleChatDetach(rr, req)
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"detached":true`) {
		t.Fatalf("detach: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotPath != "/v1/sessions/web1/detach" {
		t.Fatalf("proxied path %s", gotPath)
	}
	var sent map[string]string
	json.Unmarshal([]byte(gotBody), &sent)
	if sent["attach_id"] != "att-7" {
		t.Fatalf("forwarded body = %q", gotBody)
	}
}

func TestChatStreamForwardsAttachID(t *testing.T) {
	var gotAttach string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]string
		json.NewDecoder(r.Body).Decode(&body)
		gotAttach = body["attach_id"]
		fmt.Fprint(w, "{\"event\":\"done\"}\n{\"status\":\"ok\"}\n")
	})
	cfg := startFakeV1(t, mux)
	err := chatStreamHTTP(context.Background(), cfg.socketPath, "hi", "web1", "", "", "att-9",
		func(evt streamEvent) {})
	if err != nil {
		t.Fatalf("stream: %v", err)
	}
	if gotAttach != "att-9" {
		t.Fatalf("attach_id forwarded = %q (want att-9)", gotAttach)
	}
}

func TestHandleChatSessionEventsProxiesSSE(t *testing.T) {
	var gotPath string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/events", func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
		w.Write([]byte("event: turn_started\ndata: {\"turn_id\":\"turn-1\"}\n\n"))
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/chat/session-events?sid=web1", nil)
	s.handleChatSessionEvents(rr, req)
	if gotPath != "/v1/sessions/web1/events" {
		t.Fatalf("proxied path %q", gotPath)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/event-stream" {
		t.Fatalf("content-type %q", ct)
	}
	if !strings.Contains(rr.Body.String(), "event: turn_started") {
		t.Fatalf("relayed body = %q", rr.Body.String())
	}
}

func TestHandleChatSessionEventsRequiresSid(t *testing.T) {
	s := &server{cfg: &config{}}
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/chat/session-events", nil)
	s.handleChatSessionEvents(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("missing sid: code=%d", rr.Code)
	}
}

func TestHandleChatPrimaryGetProxiesV1(t *testing.T) {
	var gotMethod, gotPath string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/primary", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		fmt.Fprint(w, `{"agent":"gpu-mid"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	s.handleChatPrimary(rr, httptest.NewRequest(http.MethodGet, "/api/chat/primary?sid=web1", nil))
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"gpu-mid"`) {
		t.Fatalf("get: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodGet || gotPath != "/v1/sessions/web1/primary" {
		t.Fatalf("proxied %s %s", gotMethod, gotPath)
	}
}

func TestHandleChatPrimarySetProxiesV1(t *testing.T) {
	var gotMethod, gotPath, gotBody string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions/web1/primary", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		fmt.Fprint(w, `{"agent":"gpu-mid"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/primary", strings.NewReader(`{"sid":"web1","agent":"gpu-mid"}`))
	s.handleChatPrimary(rr, req)
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"gpu-mid"`) {
		t.Fatalf("set: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/sessions/web1/primary" {
		t.Fatalf("proxied %s %s", gotMethod, gotPath)
	}
	var sent map[string]string
	if json.Unmarshal([]byte(gotBody), &sent); sent["agent"] != "gpu-mid" {
		t.Fatalf("forwarded body = %q", gotBody)
	}
}

// A tab that has not sent its first message has no session id yet. The handler
// must answer "nothing pinned" locally rather than proxying /v1/sessions//primary,
// so the selector falls back to the configured default instead of erroring.
func TestHandleChatPrimaryWithoutSidReportsUnpinned(t *testing.T) {
	proxied := false
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { proxied = true })
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	s.handleChatPrimary(rr, httptest.NewRequest(http.MethodGet, "/api/chat/primary", nil))
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"agent":""`) {
		t.Fatalf("no-sid: code=%d body=%q", rr.Code, rr.Body.String())
	}
	if proxied {
		t.Fatal("must not proxy upstream without a session id")
	}
}

// Setting requires both fields; a missing agent is a 400, never a write.
func TestHandleChatPrimarySetRejectsMissingFields(t *testing.T) {
	s := &server{cfg: &config{socketPath: filepath.Join(t.TempDir(), "aimee.sock")}}
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/chat/primary", strings.NewReader(`{"sid":"web1"}`))
	s.handleChatPrimary(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%q", rr.Code, rr.Body.String())
	}
}

// A chat mutation the SPA exposes but the backend has not built must SAY so.
//
// These three returned {"status":"ok"} unconditionally while the shipped UI
// carries the controls that call them ("Rewind to here", "Rewinding…", "Branch
// from current point"). A user clicked Rewind, saw the pending state resolve to
// success, and their conversation was untouched — and the SPA's own "Rewind
// failed" branch could never fire, because the stub always claimed to have
// worked. No sockets here on purpose: the handlers answer without touching the
// v1 transport, so this exercises them directly.
func TestUnimplementedChatMutationsDoNotClaimSuccess(t *testing.T) {
	s := &server{}
	cases := []struct {
		name string
		h    http.HandlerFunc
	}{
		{"branch", s.handleChatBranch},
		{"switch-thread", s.handleChatSwitchThread},
		{"rewind", s.handleChatRewind},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rr := httptest.NewRecorder()
			tc.h(rr, httptest.NewRequest(http.MethodPost, "/api/chat/"+tc.name,
				strings.NewReader(`{}`)))

			if rr.Code != http.StatusNotImplemented {
				t.Fatalf("status = %d, want 501 (a mutation that does nothing must not report success)", rr.Code)
			}
			var body map[string]any
			if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
				t.Fatalf("response is not JSON: %v (%s)", err, rr.Body.String())
			}
			if _, ok := body["error"]; !ok {
				t.Fatalf("no error field: %s", rr.Body.String())
			}
			if got, ok := body["status"]; ok && got == "ok" {
				t.Fatalf("still reports status ok: %s", rr.Body.String())
			}
		})
	}
}

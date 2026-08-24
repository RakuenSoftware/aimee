package peer

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// testHandler wires a registry to a handler that authorizes everything, so the
// route behaviour can be tested apart from the auth scheme.
func testHandler(t *testing.T, r *Registry) http.Handler {
	t.Helper()
	return r.Handler(HTTPOptions{
		Authorize:    func(*http.Request, string) bool { return true },
		Principal:    func(*http.Request) string { return "uid:1000" },
		AdminAllowed: func(*http.Request) bool { return true },
	})
}

func do(t *testing.T, h http.Handler, method, path string, body any) (int, map[string]any) {
	t.Helper()
	var buf bytes.Buffer
	if body != nil {
		if err := json.NewEncoder(&buf).Encode(body); err != nil {
			t.Fatalf("encode: %v", err)
		}
	}
	req := httptest.NewRequest(method, path, &buf)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	var out map[string]any
	if rec.Body.Len() > 0 {
		if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil {
			t.Fatalf("decode %s %s: %v (body %q)", method, path, err, rec.Body.String())
		}
	}
	return rec.Code, out
}

// Without an Authorize hook every session route fails closed. A peer surface
// that authorizes by accident is worse than one that is switched off.
func TestHandlerFailsClosedWithoutAuthorize(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	h := r.Handler(HTTPOptions{}) // no hooks at all

	for _, tc := range []struct{ method, path string }{
		{"POST", "/v1/sessions/A/peer"},
		{"POST", "/v1/sessions/A/peer/reply"},
		{"GET", "/v1/sessions/A/inbox"},
		{"POST", "/v1/sessions/A/inbox/take"},
		{"POST", "/v1/sessions/A/label"},
		{"GET", "/v1/sessions/peers"},
	} {
		code, _ := do(t, h, tc.method, tc.path, map[string]any{})
		if code != http.StatusServiceUnavailable {
			t.Errorf("%s %s = %d; want 503 (fail closed)", tc.method, tc.path, code)
		}
	}
	// Grant management refuses rather than 503s, but it still refuses.
	if code, _ := do(t, h, "POST", "/v1/peers/grants", map[string]any{}); code != http.StatusForbidden {
		t.Errorf("grants without admin hook = %d; want 403", code)
	}
	if r.Len("B") != 0 {
		t.Error("a fail-closed handler delivered a message")
	}
}

// A caller refused by Authorize cannot act as that session.
func TestHandlerRefusesUnauthorizedSession(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	h := r.Handler(HTTPOptions{
		Authorize: func(_ *http.Request, id string) bool { return id == "B" },
		Principal: func(*http.Request) string { return "uid:1000" },
	})
	code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "B", Text: "hi"})
	if code != http.StatusForbidden {
		t.Fatalf("unauthorized send = %d; want 403", code)
	}
	if r.Len("B") != 0 {
		t.Error("an unauthorized send was delivered")
	}
}

func TestHandlerSendAndInbox(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	if err := r.SetLabel("B", "reviewer"); err != nil {
		t.Fatal(err)
	}
	h := testHandler(t, r)

	// The directory is scoped to the caller's principal.
	code, out := do(t, h, "GET", "/v1/sessions/peers", nil)
	if code != http.StatusOK {
		t.Fatalf("directory = %d", code)
	}
	if peers, _ := out["peers"].([]any); len(peers) != 2 {
		t.Fatalf("directory peers = %v", out["peers"])
	}

	// Addressing by label resolves within the sender's own owner.
	code, out = do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{ToLabel: "reviewer", Text: "hello"})
	if code != http.StatusOK {
		t.Fatalf("send by label = %d (%v)", code, out)
	}
	if out["status"] != "sent" {
		t.Errorf("status = %v", out["status"])
	}

	code, out = do(t, h, "GET", "/v1/sessions/B/inbox", nil)
	if code != http.StatusOK {
		t.Fatalf("inbox = %d", code)
	}
	msgs, _ := out["messages"].([]any)
	if len(msgs) != 1 {
		t.Fatalf("inbox messages = %v", out["messages"])
	}
	// Reading the inbox must not consume it.
	if r.Len("B") != 1 {
		t.Error("GET /inbox removed messages; it must only read")
	}

	code, out = do(t, h, "POST", "/v1/sessions/B/inbox/take", map[string]any{"max": 10})
	if code != http.StatusOK {
		t.Fatalf("take = %d", code)
	}
	if msgs, _ := out["messages"].([]any); len(msgs) != 1 {
		t.Fatalf("take returned %v", out["messages"])
	}
	if r.Len("B") != 0 {
		t.Error("take did not remove what it returned")
	}
}

// A timed-out ask is reported as a 200 with status "timeout" — it degraded to a
// send, which is not an error the caller should treat as failure.
func TestHandlerAskTimeoutIsNotAnError(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	h := testHandler(t, r)

	code, out := do(t, h, "POST", "/v1/sessions/A/peer",
		sendRequest{To: "B", Text: "anyone there?", WaitMS: 50})
	if code != http.StatusOK {
		t.Fatalf("timed-out ask = %d; want 200", code)
	}
	if out["status"] != "timeout" {
		t.Fatalf("status = %v; want timeout", out["status"])
	}
	if out["message_id"] == "" || out["message_id"] == nil {
		t.Error("a timed-out ask must report the question id to correlate on")
	}
	if r.Len("B") != 1 {
		t.Error("the question did not stay in the peer's inbox")
	}
}

func TestHandlerAskAnswered(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	h := testHandler(t, r)

	go func() {
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) {
			if got := r.Take("B", 1); len(got) == 1 {
				r.Reply("B", got[0], "yes")
				return
			}
			time.Sleep(2 * time.Millisecond)
		}
	}()

	code, out := do(t, h, "POST", "/v1/sessions/A/peer",
		sendRequest{To: "B", Text: "ship it?", WaitMS: 3000})
	if code != http.StatusOK {
		t.Fatalf("ask = %d (%v)", code, out)
	}
	if out["status"] != "answered" {
		t.Fatalf("status = %v", out["status"])
	}
	reply, _ := out["reply"].(map[string]any)
	if reply["text"] != "yes" || reply["from_session"] != "B" {
		t.Errorf("reply = %v", reply)
	}
}

// The status codes carry the distinctions a caller acts on.
func TestHandlerErrorStatuses(t *testing.T) {
	r := New(Options{})
	for _, s := range []struct{ id, owner string }{{"A", "uid:1000"}, {"B", "uid:1000"}, {"X", "uid:2000"}} {
		if err := r.Register(s.id, s.owner, "cli"); err != nil {
			t.Fatal(err)
		}
	}
	h := testHandler(t, r)

	// Cross-owner without a grant → 403.
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "X", Text: "hi"}); code != http.StatusForbidden {
		t.Errorf("cross-owner = %d; want 403", code)
	}
	// Unknown peer → 404.
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "ghost", Text: "hi"}); code != http.StatusNotFound {
		t.Errorf("unknown peer = %d; want 404", code)
	}
	// Self-addressing → 400.
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "A", Text: "hi"}); code != http.StatusBadRequest {
		t.Errorf("self = %d; want 400", code)
	}
	// Over-long body → 413, distinct from a malformed one.
	big := make([]byte, MaxTextBytes+1)
	for i := range big {
		big[i] = 'z'
	}
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "B", Text: string(big)}); code != http.StatusRequestEntityTooLarge {
		t.Errorf("over-long = %d; want 413", code)
	}
	// Inbox full → 429, a budget signal rather than a client error.
	for i := 0; i < InboxMax; i++ {
		if _, err := r.Send("A", "B", "fill", SendOptions{}); err != nil {
			t.Fatal(err)
		}
	}
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "B", Text: "over"}); code != http.StatusTooManyRequests {
		t.Errorf("inbox full = %d; want 429", code)
	}
	// Label collision: 409. Labels ARE this module's state, since
	// server_sessions.title has no writer at all, so the route exists here and
	// enforces per-owner uniqueness.
	if err := r.SetLabel("A", "taken"); err != nil {
		t.Fatal(err)
	}
	if code, _ := do(t, h, "POST", "/v1/sessions/B/label", map[string]any{"label": "taken"}); code != http.StatusConflict {
		t.Errorf("label collision = %d; want 409", code)
	}
}

func TestHandlerGrants(t *testing.T) {
	r := New(Options{})
	if err := r.Register("A", "uid:1000", "cli"); err != nil {
		t.Fatal(err)
	}
	if err := r.Register("X", "uid:2000", "cli"); err != nil {
		t.Fatal(err)
	}
	h := testHandler(t, r)

	code, out := do(t, h, "POST", "/v1/peers/grants",
		map[string]any{"from_owner": "uid:1000", "to_owner": "uid:2000"})
	if code != http.StatusOK || out["status"] != "granted" {
		t.Fatalf("grant = %d %v", code, out)
	}
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "X", Text: "hi"}); code != http.StatusOK {
		t.Errorf("granted send = %d; want 200", code)
	}
	// Directed: the reverse still refuses.
	if r.GrantExists("uid:2000", "uid:1000") {
		t.Error("grant leaked in reverse")
	}

	code, out = do(t, h, "POST", "/v1/peers/grants",
		map[string]any{"from_owner": "uid:1000", "to_owner": "uid:2000", "revoke": true})
	if code != http.StatusOK || out["existed"] != true {
		t.Fatalf("revoke = %d %v", code, out)
	}
	if code, _ := do(t, h, "POST", "/v1/sessions/A/peer", sendRequest{To: "X", Text: "hi"}); code != http.StatusForbidden {
		t.Errorf("after revoke = %d; want 403", code)
	}
}

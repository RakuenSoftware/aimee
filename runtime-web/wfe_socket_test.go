package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

func TestWorkflowFamilyAlwaysUsesCanonicalFacade(t *testing.T) {
	t.Setenv("AIMEE_WFE_ENGINE", "go")
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", filepath.Join(t.TempDir(), "private-go.sock"))
	cfg, err := newConfig(8080, "", "", filepath.Join(t.TempDir(), "aimee.sock"), filepath.Join(t.TempDir(), "webchat.db"), "")
	if err != nil {
		t.Fatal(err)
	}
	s := &server{cfg: cfg}
	want := s.aimeeHTTPSockPath()
	for _, path := range []string{"/v1/workflow", "/v1/workflow/items", "/v1/workflow/config/set", "/v1/trigger/fire", "/v1/dev/submit", "/v1/delegate/run", "/v1/workflowX"} {
		if got := s.aimeeHTTPSockPathFor(path); got != want {
			t.Fatalf("%s bypassed canonical admission: %s", path, got)
		}
	}
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", filepath.Join(t.TempDir(), "changed-private.sock"))
	if got := s.aimeeHTTPSockPathFor("/v1/dev/submit"); got != want {
		t.Fatalf("ambient private socket redirected ingress: %s", got)
	}
}
func TestNormalDevSubmitCrossesCanonicalFacadeWithAuthenticatedIdentity(t *testing.T) {
	privateCalls, facadeCalls := 0, 0
	privateCfg := startFakeV1(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		privateCalls++
		_, _ = w.Write([]byte(`{"ok":true,"work_item_id":"bypassed"}`))
	}))
	t.Setenv("AIMEE_WFE_ENGINE", "go")
	t.Setenv("AIMEE_WFE_HTTP_SOCKET", (&server{cfg: privateCfg}).aimeeHTTPSockPath())
	const body = `{"proposal_md":"owned proposal","repo":"/approved"}`
	facadeCfg := startFakeV1(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		facadeCalls++
		got, _ := io.ReadAll(r.Body)
		if r.Method != "POST" || r.URL.Path != "/v1/dev/submit" || string(got) != body || r.Header.Get("X-Aimee-Webuser") != "alice" || r.Header.Get("X-Aimee-Workflow-Operator") != "" || r.Header.Get("X-Aimee-Execution-Scope") != "" {
			t.Errorf("facade request identity/body lost or spoofed: %s %s headers=%v", r.Method, r.URL.Path, r.Header)
		}
		_, _ = w.Write([]byte(`{"ok":true,"work_item_id":"owned"}`))
	}))
	cfg, err := newConfig(8080, "", "", facadeCfg.socketPath, filepath.Join(t.TempDir(), "webchat.db"), "")
	if err != nil {
		t.Fatal(err)
	}
	s := &server{cfg: cfg}
	request := withUser(httptest.NewRequest("POST", "/api/dev/submit", strings.NewReader(body)), "alice")
	request.Header.Set("X-Aimee-Webuser", "forged")
	request.Header.Set("X-Aimee-Workflow-Operator", "true")
	request.Header.Set("X-Aimee-Execution-Scope", strings.Repeat("b", 64))
	response := httptest.NewRecorder()
	s.handleDevSubmit(response, request)
	waitFakeV1(t)
	if response.Code != 200 || facadeCalls != 1 || privateCalls != 0 || !strings.Contains(response.Body.String(), `"owned"`) {
		t.Fatalf("normal public submit bypassed owner: status=%d facade=%d private=%d", response.Code, facadeCalls, privateCalls)
	}
}

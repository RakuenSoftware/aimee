package git

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// forgeStub stands in for the forge and records what it was asked, so the tests
// assert the REQUEST as well as the parse. Getting the endpoint or the method
// wrong is not visibly different from a forge that said no.
type forgeStub struct {
	method, path, auth, accept, body string
	status                           int
	reply                            string
}

func (s *forgeStub) start(t *testing.T) {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.method, s.path = r.Method, r.URL.RequestURI()
		s.auth, s.accept = r.Header.Get("Authorization"), r.Header.Get("Accept")
		buf := make([]byte, r.ContentLength)
		if r.ContentLength > 0 {
			_, _ = r.Body.Read(buf)
		}
		s.body = string(buf)
		if s.status == 0 {
			s.status = 200
		}
		w.WriteHeader(s.status)
		_, _ = w.Write([]byte(s.reply))
	}))
	previous := forgeBaseURL
	forgeBaseURL = server.URL
	t.Cleanup(func() { forgeBaseURL = previous; server.Close() })
}

func TestForgeSendsTheCredentialOnlyInTheHeader(t *testing.T) {
	stub := &forgeStub{reply: `{"default_branch":"testing"}`}
	stub.start(t)

	out := PerformForge(ForgeRequest{Op: OpDefaultBranch, Owner: "o", Repo: "r", Token: "s3cret"})
	if out.Error != "" {
		t.Fatalf("unexpected error: %s", out.Error)
	}
	if stub.auth != "Bearer s3cret" {
		t.Fatalf("Authorization = %q", stub.auth)
	}
	// The token must never reach the URL (proxies and logs keep those) nor the
	// response the caller gets back.
	if got := stub.path; got != "/repos/o/r" || strings.Contains(got, "s3cret") {
		t.Fatalf("path = %q, want the bare repo endpoint with no credential", got)
	}
	encoded, _ := json.Marshal(out)
	if strings.Contains(string(encoded), "s3cret") {
		t.Fatalf("the credential must not appear in the response: %s", encoded)
	}
	if stub.accept != forgeAccept {
		t.Fatalf("Accept = %q", stub.accept)
	}
	// The DEFAULT BRANCH IS AUTHORITATIVE: a caller must never fall back to
	// "main", so it has to be reported exactly as the forge stated it.
	if out.DefaultBranch != "testing" {
		t.Fatalf("default branch = %q, want testing", out.DefaultBranch)
	}
}

// The bare form matters: GitHub 404s /repos/o/r/ with a trailing slash.
func TestDefaultBranchUsesTheBareRepoEndpoint(t *testing.T) {
	stub := &forgeStub{reply: `{"default_branch":"main"}`}
	stub.start(t)
	PerformForge(ForgeRequest{Op: OpDefaultBranch, Owner: "o", Repo: "r", Token: "t"})
	if stub.path != "/repos/o/r" {
		t.Fatalf("path = %q, want /repos/o/r with no trailing slash", stub.path)
	}
}

func TestPRCreateSendsTheFieldsAndReadsTheNumber(t *testing.T) {
	stub := &forgeStub{status: 201, reply: `{"number":42,"state":"open","title":"t",
		"head":{"ref":"feat"},"base":{"ref":"testing"},"draft":true,"html_url":"u"}`}
	stub.start(t)

	out := PerformForge(ForgeRequest{
		Op: OpPRCreate, Owner: "o", Repo: "r", Token: "t",
		Title: "t", Head: "feat", Base: "testing", Body: "b", Draft: true,
	})
	if out.Error != "" || out.Pull == nil {
		t.Fatalf("unexpected: %+v", out)
	}
	if stub.method != http.MethodPost || stub.path != "/repos/o/r/pulls" {
		t.Fatalf("%s %s", stub.method, stub.path)
	}
	for _, want := range []string{`"head":"feat"`, `"base":"testing"`, `"draft":true`} {
		if !strings.Contains(stub.body, want) {
			t.Fatalf("request body %s missing %s", stub.body, want)
		}
	}
	if out.Pull.Number != 42 || out.Pull.Head != "feat" || out.Pull.Base != "testing" {
		t.Fatalf("summary = %+v", out.Pull)
	}
}

// A refusal must arrive as the forge's own message. "HTTP 422" alone sends an
// operator hunting; "A pull request already exists" does not.
func TestForgeRefusalCarriesTheForgeMessage(t *testing.T) {
	stub := &forgeStub{status: 422, reply: `{"message":"A pull request already exists"}`}
	stub.start(t)
	out := PerformForge(ForgeRequest{Op: OpPRCreate, Owner: "o", Repo: "r", Token: "t"})
	if out.Status != 422 {
		t.Fatalf("status = %d, want 422", out.Status)
	}
	if !strings.Contains(out.Error, "A pull request already exists") || !strings.Contains(out.Error, "422") {
		t.Fatalf("error = %q, want the forge message and the status", out.Error)
	}
	if out.Pull != nil {
		t.Fatal("a refused create must not report a pull")
	}
}

// Status 0 with an error means we never reached the forge. A caller that cannot
// tell that from a refusal will report a network blip as a rejected merge.
func TestTransportFailureIsNotAForgeRefusal(t *testing.T) {
	previous := forgeBaseURL
	forgeBaseURL = "http://127.0.0.1:1" // nothing listens
	t.Cleanup(func() { forgeBaseURL = previous })

	out := PerformForge(ForgeRequest{Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 1})
	if out.Status != 0 || out.Error == "" {
		t.Fatalf("want status 0 with an error, got %+v", out)
	}
	if out.Merged {
		t.Fatal("an unreachable forge must never report a merge")
	}
}

func TestPRMergeReportsWhatTheForgeSaid(t *testing.T) {
	stub := &forgeStub{reply: `{"merged":true}`}
	stub.start(t)
	out := PerformForge(ForgeRequest{Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 7})
	if stub.method != http.MethodPut || stub.path != "/repos/o/r/pulls/7/merge" {
		t.Fatalf("%s %s", stub.method, stub.path)
	}
	if !out.Merged || out.Error != "" {
		t.Fatalf("out = %+v", out)
	}

	// 409 is the forge refusing on conflict; it must not read as merged.
	stub2 := &forgeStub{status: 409, reply: `{"message":"Merge conflict"}`}
	stub2.start(t)
	out = PerformForge(ForgeRequest{Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 7})
	if out.Merged || out.Status != 409 || !strings.Contains(out.Error, "Merge conflict") {
		t.Fatalf("out = %+v", out)
	}
}

func TestPRFindOpenQualifiesTheHeadWithTheOwner(t *testing.T) {
	stub := &forgeStub{reply: `[{"number":5,"state":"open","head":{"ref":"feat"}}]`}
	stub.start(t)
	out := PerformForge(ForgeRequest{
		Op: OpPRFindOpen, Owner: "acme", Repo: "r", Token: "t", Head: "feat",
	})
	// Unqualified, GitHub's head filter matches nothing and the caller concludes
	// there is no open PR — then opens a duplicate.
	if !strings.Contains(stub.path, "head=acme%3Afeat") {
		t.Fatalf("path = %q, want an owner-qualified head filter", stub.path)
	}
	if out.Pull == nil || out.Pull.Number != 5 {
		t.Fatalf("out = %+v", out)
	}
}

func TestGuardsRejectBadInputBeforeAnyCall(t *testing.T) {
	previous := forgeBaseURL
	forgeBaseURL = "http://127.0.0.1:1" // any call would fail loudly
	t.Cleanup(func() { forgeBaseURL = previous })

	for _, tc := range []struct {
		name string
		req  ForgeRequest
	}{
		{"no token", ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "r", Number: 1}},
		{"bad owner", ForgeRequest{Op: OpPRInfo, Owner: "o/../x", Repo: "r", Token: "t", Number: 1}},
		{"bad repo", ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "..", Token: "t", Number: 1}},
		{"no number", ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "r", Token: "t"}},
		{"unknown op", ForgeRequest{Op: "delete_everything", Owner: "o", Repo: "r", Token: "t"}},
	} {
		out := PerformForge(tc.req)
		if out.Error == "" {
			t.Fatalf("%s: expected a refusal before any request", tc.name)
		}
		if out.Status != 0 {
			t.Fatalf("%s: nothing should have been sent, got status %d", tc.name, out.Status)
		}
	}
}

func TestHandleForgeRequestRoutesThroughStageFour(t *testing.T) {
	stub := &forgeStub{reply: `{"default_branch":"testing"}`}
	stub.start(t)

	request, err := json.Marshal(ForgeRequest{
		Op: OpDefaultBranch, Owner: "o", Repo: "r", Token: "t",
	})
	if err != nil {
		t.Fatal(err)
	}
	body, status := Handle(bus.ModuleInvocation{StageID: StageForgeRequest}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded ForgeResponse
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.DefaultBranch != "testing" {
		t.Fatalf("decoded = %+v", decoded)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageForgeRequest}, []byte("{")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed JSON must be rejected, got %v", status)
	}
}

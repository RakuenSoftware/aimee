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

// MERGEABLE IS THREE-VALUED. The forge answers null while it is still computing
// the merge, which is NOT "cannot merge". Flattening null to false tells a
// caller a perfectly mergeable PR is conflicted, and it abandons a merge that
// would have succeeded a moment later — so absent, true and false are asserted
// as three distinct outcomes.
func TestPRInfoKeepsMergeableThreeValued(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want *bool
	}{
		{"still computing", `{"number":1,"state":"open","mergeable":null,
			"head":{"ref":"f","sha":"abc"},"base":{"ref":"testing"}}`, nil},
		{"mergeable", `{"number":1,"state":"open","mergeable":true,
			"head":{"ref":"f","sha":"abc"},"base":{"ref":"testing"}}`, boolPtr(true)},
		{"not mergeable", `{"number":1,"state":"open","mergeable":false,
			"head":{"ref":"f","sha":"abc"},"base":{"ref":"testing"}}`, boolPtr(false)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			stub := &forgeStub{reply: tc.body}
			stub.start(t)
			out := PerformForge(ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "r", Token: "t", Number: 1})
			if out.Error != "" || out.Pull == nil {
				t.Fatalf("unexpected: %+v", out)
			}
			got := out.Pull.Mergeable
			if (got == nil) != (tc.want == nil) {
				t.Fatalf("mergeable = %v, want %v", got, tc.want)
			}
			if got != nil && *got != *tc.want {
				t.Fatalf("mergeable = %v, want %v", *got, *tc.want)
			}
			// And it must survive the wire the same way.
			encoded, _ := json.Marshal(out.Pull)
			hasKey := strings.Contains(string(encoded), `"mergeable"`)
			if hasKey != (tc.want != nil) {
				t.Fatalf("encoded %s: mergeable key present=%v, want %v", encoded, hasKey, tc.want != nil)
			}
		})
	}
}

func TestPRInfoCarriesTheRefsAndStateACallerNeeds(t *testing.T) {
	stub := &forgeStub{reply: `{"number":7,"state":"closed","title":"T","merged":true,
		"merged_at":"2026-08-10T13:17:33Z","mergeable_state":"clean","html_url":"u",
		"head":{"ref":"feat","sha":"deadbeef"},"base":{"ref":"testing"}}`}
	stub.start(t)
	out := PerformForge(ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "r", Token: "t", Number: 7})
	if out.Error != "" || out.Pull == nil {
		t.Fatalf("unexpected: %+v", out)
	}
	p := out.Pull
	// head_sha is what drift-safety on a merge is checked against, so losing it
	// silently disables that protection.
	if p.HeadSHA != "deadbeef" || p.Head != "feat" || p.Base != "testing" {
		t.Fatalf("refs = %+v", p)
	}
	if !p.Merged || p.MergedAt != "2026-08-10T13:17:33Z" {
		t.Fatalf("merge facts = %+v", p)
	}
	// REST spells it lowercase; callers render the upper-cased gh spelling, and
	// normalising here means no caller has to remember to.
	if p.MergeState != "CLEAN" {
		t.Fatalf("merge_state = %q, want CLEAN", p.MergeState)
	}
}

// A PR that was never merged has merged_at null; it must not become the string
// "null" or an empty-but-present value a caller might render.
func TestNeverMergedHasNoMergedAt(t *testing.T) {
	stub := &forgeStub{reply: `{"number":1,"state":"open","merged":false,"merged_at":null,
		"head":{"ref":"f","sha":"abc"},"base":{"ref":"testing"}}`}
	stub.start(t)
	out := PerformForge(ForgeRequest{Op: OpPRInfo, Owner: "o", Repo: "r", Token: "t", Number: 1})
	if out.Pull == nil || out.Pull.MergedAt != "" {
		t.Fatalf("merged_at = %+v, want empty", out.Pull)
	}
	encoded, _ := json.Marshal(out.Pull)
	if strings.Contains(string(encoded), "merged_at") {
		t.Fatalf("merged_at must be omitted entirely: %s", encoded)
	}
}

func boolPtr(v bool) *bool { return &v }

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

// The 405/409 pair is the subtle one. A CONTENT conflict is a property of the
// two trees: retrying reproduces it exactly, so it is terminal. A moved
// head/base is a lost race a retry wins. Both arrive with the same statuses and
// the message is the only discriminator the forge gives, so misreading it either
// terminates a run that would have succeeded or spins on one that never will.
func TestMergeSeparatesTerminalConflictFromLostRace(t *testing.T) {
	for _, tc := range []struct {
		name            string
		status          int
		reply           string
		conflict, retry bool
		alreadyMerged   bool
	}{
		{"content conflict is terminal", 409,
			`{"message":"Pull Request has merge conflicts"}`, true, false, false},
		// HTTP 409 is literally named "Conflict"; the bare word must NOT
		// terminate, or every lost race is misread as unmergeable.
		{"bare conflict wording is a lost race", 409,
			`{"message":"Conflict: head branch was modified"}`, false, true, false},
		{"405 lost race", 405, `{"message":"Base branch was modified"}`, false, true, false},
		{"already merged", 405, `{"message":"Pull Request is already merged"}`, false, false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			stub := &forgeStub{status: tc.status, reply: tc.reply}
			stub.start(t)
			out := PerformForge(ForgeRequest{
				Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 3,
			})
			if out.Merged {
				t.Fatal("a refused merge must never report merged")
			}
			if out.Conflict != tc.conflict || out.Retryable != tc.retry ||
				out.AlreadyMerged != tc.alreadyMerged {
				t.Fatalf("conflict=%v retryable=%v already=%v, want %v/%v/%v",
					out.Conflict, out.Retryable, out.AlreadyMerged,
					tc.conflict, tc.retry, tc.alreadyMerged)
			}
		})
	}
}

func TestMergeCarriesMethodDriftGuardAndReturnsTheSHA(t *testing.T) {
	stub := &forgeStub{reply: `{"merged":true,"sha":"abc123"}`}
	stub.start(t)
	out := PerformForge(ForgeRequest{
		Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 3,
		MergeMethod: "squash", ExpectedHeadSHA: "deadbeef",
	})
	if !out.Merged || out.MergeSHA != "abc123" {
		t.Fatalf("out = %+v", out)
	}
	// Only a squash synthesises a body from the child commits, which is where the
	// attribution trailers protected branches reject come back in.
	if !strings.Contains(stub.body, `"merge_method":"squash"`) ||
		!strings.Contains(stub.body, `"commit_message":""`) {
		t.Fatalf("squash body = %s", stub.body)
	}
	// Drift safety: the forge refuses if the head moved since it was read.
	if !strings.Contains(stub.body, `"sha":"deadbeef"`) {
		t.Fatalf("body missing the expected head sha: %s", stub.body)
	}

	// A plain merge has nothing to synthesise, so no commit_message is sent.
	stub2 := &forgeStub{reply: `{"merged":true,"sha":"x"}`}
	stub2.start(t)
	PerformForge(ForgeRequest{Op: OpPRMerge, Owner: "o", Repo: "r", Token: "t", Number: 3})
	if strings.Contains(stub2.body, "commit_message") {
		t.Fatalf("a plain merge must not synthesise a message: %s", stub2.body)
	}
	if !strings.Contains(stub2.body, `"merge_method":"merge"`) {
		t.Fatalf("default method = %s", stub2.body)
	}
}

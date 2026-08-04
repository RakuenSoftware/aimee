package api

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	appconfig "github.com/JBailes/aimee/server-go/internal/config"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func newTestServer(t *testing.T) (*Server, *db1.Store, *wfe.ArtifactStore) {
	t.Helper()
	root := t.TempDir()
	store, err := db1.Open(filepath.Join(root, "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	artifacts, err := wfe.NewArtifactStore(filepath.Join(root, "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	server, err := New(store, artifacts)
	if err != nil {
		t.Fatal(err)
	}
	return server, store, artifacts
}

func setWorkflowIdentity(req *http.Request, user string, operator bool) {
	req.Header.Set("X-Aimee-Webuser", user)
	if operator {
		req.Header.Set("X-Aimee-Workflow-Operator", "true")
	}
}

func TestProposalEndpointImportsLegacySourceWithoutTruncation(t *testing.T) {
	server, store, _ := newTestServer(t)
	tail := "ACCEPTANCE_CRITERION_AFTER_ALL_PRIOR_BYTE_LIMITS"
	proposal := strings.Repeat("complete proposal paragraph λ\n", 200_000) + tail
	source := filepath.Join(t.TempDir(), "proposal.md")
	if err := os.WriteFile(source, []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(context.Background(), db1.CreateWorkItem{
		ID: "wi_api", Repo: "repo", ProposalPath: source, WorkflowName: "build",
		WorkflowVersion: strings.Repeat("a", 64), StartStage: "plan", Mode: "autonomous", Submitter: "alice",
	}); err != nil {
		t.Fatal(err)
	}

	req := httptest.NewRequest(http.MethodGet, "/v1/workflow/items/wi_api/proposal", nil)
	setWorkflowIdentity(req, "alice", false)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		Proposal  string `json:"proposal_md"`
		Truncated bool   `json:"truncated"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.Truncated || response.Proposal != proposal || !strings.HasSuffix(response.Proposal, tail) {
		t.Fatal("proposal endpoint did not return the complete immutable source")
	}

	// The imported copy is authoritative after the first request.
	if err := os.WriteFile(source, []byte("mutated legacy source"), 0o600); err != nil {
		t.Fatal(err)
	}
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.Proposal != proposal {
		t.Fatal("mutable legacy source changed the imported proposal")
	}
}

func TestHealthIdentifiesGoImplementation(t *testing.T) {
	server, _, _ := newTestServer(t)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"implementation":"go"`) {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestWorkflowStopCancelsAndStopsEveryDescendant(t *testing.T) {
	server, store, _ := newTestServer(t)
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_api_stop", Repo: "repo", ProposalPath: "root", WorkflowName: "build", StartStage: "slices", Submitter: "alice"},
		{ID: "wi_api_stop.child", Repo: "repo", ProposalPath: "child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_api_stop"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}
	cancelled := make(map[string]bool)
	server.SetSchedulerCancel(func(id string) { cancelled[id] = true })
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/v1/workflow/items/wi_api_stop/stop", nil)
	setWorkflowIdentity(req, "alice", false)
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	for _, id := range []string{"wi_api_stop", "wi_api_stop.child"} {
		item, err := store.WorkItem(ctx, id)
		if err != nil || item.State != "stopped" {
			t.Fatalf("%s item=%+v err=%v", id, item, err)
		}
		if !cancelled[id] {
			t.Fatalf("scheduler did not cancel %s", id)
		}
	}
}

func TestWorkflowItemsAreScopedToRootSubmitterAndOperator(t *testing.T) {
	server, store, _ := newTestServer(t)
	ctx := context.Background()
	for _, in := range []db1.CreateWorkItem{
		{ID: "wi_alice", Repo: "repo", ProposalPath: "alice", WorkflowName: "build", StartStage: "plan", Submitter: "alice"},
		// Older child slices have no submitter of their own. Ownership must follow
		// the durable parent chain to the root instead of hiding or exposing them.
		{ID: "wi_alice.s0", Repo: "repo", ProposalPath: "alice-child", WorkflowName: "slice", StartStage: "impl", ParentID: "wi_alice"},
		{ID: "wi_bob", Repo: "repo", ProposalPath: "bob", WorkflowName: "build", StartStage: "plan", Submitter: "bob"},
		// Trigger-origin runs have no browser submitter and are operator-only.
		{ID: "wi_trigger", Repo: "repo", ProposalPath: "trigger", WorkflowName: "build", StartStage: "plan"},
	} {
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
	}

	request := func(method, target, user, body string) *httptest.ResponseRecorder {
		t.Helper()
		req := httptest.NewRequest(method, target, strings.NewReader(body))
		setWorkflowIdentity(req, user, user == "admin")
		rec := httptest.NewRecorder()
		server.ServeHTTP(rec, req)
		return rec
	}
	ids := func(rec *httptest.ResponseRecorder) map[string]bool {
		t.Helper()
		var response struct {
			Items []db1.WorkItem `json:"items"`
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
			t.Fatal(err)
		}
		result := make(map[string]bool, len(response.Items))
		for _, item := range response.Items {
			result[item.ID] = true
		}
		return result
	}

	aliceList := request(http.MethodGet, "/v1/workflow/items", "alice", "")
	if aliceList.Code != http.StatusOK {
		t.Fatalf("alice list status=%d body=%s", aliceList.Code, aliceList.Body.String())
	}
	if got := ids(aliceList); len(got) != 2 || !got["wi_alice"] || !got["wi_alice.s0"] {
		t.Fatalf("alice visible items=%v, want root and inherited child only", got)
	}
	if rec := request(http.MethodGet, "/v1/workflow/items/all", "alice", ""); rec.Code != http.StatusForbidden {
		t.Fatalf("non-operator all-items status=%d body=%s", rec.Code, rec.Body.String())
	}
	// The operator role is not inferred from username data. This remains
	// important after the bootstrap account is renamed and "admin" can be an
	// unrelated authenticated identity.
	literalAdmin := httptest.NewRequest(http.MethodGet, "/v1/workflow/items/wi_alice", nil)
	setWorkflowIdentity(literalAdmin, "admin", false)
	literalAdminRec := httptest.NewRecorder()
	server.ServeHTTP(literalAdminRec, literalAdmin)
	if literalAdminRec.Code != http.StatusForbidden {
		t.Fatalf("literal admin username status=%d body=%s", literalAdminRec.Code, literalAdminRec.Body.String())
	}
	operatorList := request(http.MethodGet, "/v1/workflow/items/all", "admin", "")
	if operatorList.Code != http.StatusOK || len(ids(operatorList)) != 4 {
		t.Fatalf("operator items status=%d body=%s", operatorList.Code, operatorList.Body.String())
	}
	if rec := request(http.MethodGet, "/v1/workflow/items/wi_alice.s0", "alice", ""); rec.Code != http.StatusOK {
		t.Fatalf("inherited child ownership status=%d body=%s", rec.Code, rec.Body.String())
	}
	if rec := request(http.MethodGet, "/v1/workflow/items/wi_trigger", "admin", ""); rec.Code != http.StatusOK {
		t.Fatalf("operator trigger detail status=%d body=%s", rec.Code, rec.Body.String())
	}

	for _, tc := range []struct {
		name, method, target, body string
	}{
		{"detail", http.MethodGet, "/v1/workflow/items/wi_alice", ""},
		{"events", http.MethodGet, "/v1/workflow/items/wi_alice/events", ""},
		{"proposal", http.MethodGet, "/v1/workflow/items/wi_alice/proposal", ""},
		{"pause", http.MethodPost, "/v1/workflow/items/wi_alice/pause", ""},
		{"resume", http.MethodPost, "/v1/workflow/items/wi_alice/resume", ""},
		{"stop", http.MethodPost, "/v1/workflow/items/wi_alice/stop", ""},
		{"delete", http.MethodDelete, "/v1/workflow/items/wi_alice", ""},
	} {
		t.Run("cross-user-"+tc.name, func(t *testing.T) {
			if rec := request(tc.method, tc.target, "bob", tc.body); rec.Code != http.StatusForbidden {
				t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
			}
		})
	}
	if rec := request(http.MethodPost, "/v1/workflow/items/wi_alice/gate", "alice", `{"decision":"approve"}`); rec.Code != http.StatusForbidden {
		t.Fatalf("owner gate decision status=%d body=%s", rec.Code, rec.Body.String())
	}
	item, err := store.WorkItem(ctx, "wi_alice")
	if err != nil || item.State != "active" || item.PauseReason != "" {
		t.Fatalf("unauthorized lifecycle calls mutated item=%+v err=%v", item, err)
	}
}

func TestWorkflowDefinitionWritesRequireAdministrator(t *testing.T) {
	server, _, _ := newTestServer(t)
	server.workflowDir = t.TempDir()

	request := func(method, target, user, body string) *httptest.ResponseRecorder {
		t.Helper()
		req := httptest.NewRequest(method, target, strings.NewReader(body))
		setWorkflowIdentity(req, user, user == "admin")
		rec := httptest.NewRecorder()
		server.ServeHTTP(rec, req)
		return rec
	}
	for _, user := range []string{"alice", "admin"} {
		rec := request(http.MethodGet, "/v1/workflow/blocks", user, "")
		if rec.Code != http.StatusOK {
			t.Fatalf("blocks user=%q status=%d body=%s", user, rec.Code, rec.Body.String())
		}
		var response struct {
			Editable bool `json:"editable"`
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
			t.Fatal(err)
		}
		if response.Editable != (user == "admin") {
			t.Fatalf("blocks user=%q editable=%t", user, response.Editable)
		}
	}
	for _, tc := range []struct {
		method, target, body string
	}{
		{http.MethodPut, "/v1/workflow/blocks/custom.test", `{}`},
		{http.MethodDelete, "/v1/workflow/blocks/custom.test", ``},
		{http.MethodPost, "/v1/workflow/save", `{}`},
	} {
		if rec := request(tc.method, tc.target, "alice", tc.body); rec.Code != http.StatusForbidden {
			t.Fatalf("%s %s status=%d body=%s", tc.method, tc.target, rec.Code, rec.Body.String())
		}
	}
}

func TestWorkflowMutationBodiesAndDirectTriggersAreStrict(t *testing.T) {
	server, _, _ := newTestServer(t)
	server.workflowDir = t.TempDir()

	for _, tc := range []struct {
		name, method, target, user, body string
	}{
		{"trigger-unknown-field", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","surprise":true}`},
		{"trigger-trailing-json", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md"} {}`},
		{"trigger-invalid-mode", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","mode":"manual"}`},
		{"trigger-relative-workspace", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"repo","proposal":"p.md"}`},
		{"trigger-traversal", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","event":"../private"}`},
		{"trigger-backslash", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","event":"docs\\private"}`},
		{"trigger-option-ref", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","ref":"--all"}`},
		{"trigger-negative-spend", http.MethodPost, "/v1/trigger/fire", "admin", `{"source":"watch-dir","workspace":"/repo","proposal":"p.md","max_spend_usd":-1}`},
		{"submit-trailing-json", http.MethodPost, "/v1/dev/submit", "alice", `{"proposal_md":"# Request","repo":"/repo"} {}`},
		{"gate-trailing-json", http.MethodPost, "/v1/workflow/items/missing/gate", "admin", `{"decision":"approve"} {}`},
		{"block-trailing-json", http.MethodPut, "/v1/workflow/blocks/custom.test", "admin", `{} {}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(tc.method, tc.target, strings.NewReader(tc.body))
			setWorkflowIdentity(req, tc.user, tc.user == "admin")
			rec := httptest.NewRecorder()
			server.ServeHTTP(rec, req)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
			}
		})
	}
}

func TestManualSubmissionIdempotencyIsScopedToSubmitter(t *testing.T) {
	alice := manualSubmissionIdentity("alice", "client-request-1", "build")
	if alice != manualSubmissionIdentity("alice", "client-request-1", "build") {
		t.Fatal("same submitter and key did not produce a stable identity")
	}
	if alice == manualSubmissionIdentity("bob", "client-request-1", "build") {
		t.Fatal("different submitters shared one idempotency identity")
	}
	if alice == manualSubmissionIdentity("alice", "client-request-2", "build") {
		t.Fatal("different keys shared one idempotency identity")
	}
}

func TestBearerAuthentication(t *testing.T) {
	server, _, _ := newTestServer(t)
	handler := RequireBearer(server, "secret-token")
	request := httptest.NewRequest(http.MethodGet, "/v1/health", nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("missing bearer status=%d", recorder.Code)
	}
	request = httptest.NewRequest(http.MethodGet, "/v1/health", nil)
	request.Header.Set("Authorization", "Bearer secret-token")
	recorder = httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("valid bearer status=%d body=%s", recorder.Code, recorder.Body.String())
	}
}

func TestWorkflowTriggerRegistryRoundTripFromBrowserContract(t *testing.T) {
	server, _, _ := newTestServer(t)
	configPath := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(configPath, []byte("provider: codex\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	configStore, err := appconfig.NewStore(configPath)
	if err != nil {
		t.Fatal(err)
	}
	server.SetConfigStore(configStore)

	get := func(user string) map[string]any {
		t.Helper()
		req := httptest.NewRequest(http.MethodGet, "/v1/workflow/triggers", nil)
		setWorkflowIdentity(req, user, user == "admin")
		rec := httptest.NewRecorder()
		server.ServeHTTP(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("GET triggers status=%d body=%s", rec.Code, rec.Body.String())
		}
		var response map[string]any
		if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
			t.Fatal(err)
		}
		return response
	}

	initial := get("admin")
	version, _ := initial["version"].(string)
	if version == "" || initial["operator"] != true || initial["editable"] != true || initial["max_rules"] != float64(appconfig.MaxTriggerRules) {
		t.Fatalf("initial trigger registry metadata = %#v", initial)
	}
	rules := []map[string]any{{
		"source": "watch-dir", "event": "docs/requests", "schedule": "testing",
		"mode":     "interactive",
		"pipeline": map[string]any{"template": "build", "workspace": "/srv/repos/demo", "max_spend_usd": 4.5},
	}}
	body, _ := json.Marshal(map[string]any{"key": "trigger_rules", "value": rules, "previous_version": version})
	req := httptest.NewRequest(http.MethodPost, "/v1/workflow/config/set", bytes.NewReader(body))
	setWorkflowIdentity(req, "admin", true)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("save registry status=%d body=%s", rec.Code, rec.Body.String())
	}

	saved := get("admin")
	triggers, _ := saved["triggers"].([]any)
	if len(triggers) != 1 {
		t.Fatalf("saved triggers = %#v", saved)
	}
	trigger, _ := triggers[0].(map[string]any)
	if trigger["event"] != "docs/requests" || trigger["template"] != "build" || trigger["origin"] != "config" || trigger["max_spend_usd"] != 4.5 {
		t.Fatalf("saved trigger = %#v", trigger)
	}
	content, _ := os.ReadFile(configPath)
	if !strings.Contains(string(content), "provider: codex") {
		t.Fatalf("unrelated config was lost:\n%s", content)
	}
	if ordinary := get("alice"); ordinary["operator"] != false || ordinary["editable"] != false {
		t.Fatal("non-administrator registry was advertised as editable")
	}

	staleReq := httptest.NewRequest(http.MethodPost, "/v1/workflow/config/set", bytes.NewReader(body))
	setWorkflowIdentity(staleReq, "admin", true)
	staleRec := httptest.NewRecorder()
	server.ServeHTTP(staleRec, staleReq)
	if staleRec.Code != http.StatusConflict || !strings.Contains(staleRec.Body.String(), "version conflict") {
		t.Fatalf("stale save status=%d body=%s", staleRec.Code, staleRec.Body.String())
	}
}

func TestWorkflowOperatorCapabilityIsIndependentOfTriggerRegistryAvailability(t *testing.T) {
	server, _, _ := newTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/v1/workflow/triggers", nil)
	setWorkflowIdentity(req, "renamed-owner", true)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		Operator bool `json:"operator"`
		Editable bool `json:"editable"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if !response.Operator || response.Editable {
		t.Fatalf("operator=%t editable=%t, want true/false", response.Operator, response.Editable)
	}
}

func TestWorkflowTriggerRegistryRejectsUnsafeWrites(t *testing.T) {
	server, _, _ := newTestServer(t)
	configStore, _ := appconfig.NewStore(filepath.Join(t.TempDir(), "aimee.yaml"))
	server.SetConfigStore(configStore)
	version, _ := configStore.Version("trigger_rules")
	validRule := `[{"source":"watch-dir","pipeline":{"template":"build","workspace":"/repo"}}]`

	cases := []struct {
		name, user, body string
		operator         bool
		status           int
	}{
		{"non-admin", "alice", `{"key":"trigger_rules","value":` + validRule + `,"previous_version":"` + version + `"}`, false, http.StatusForbidden},
		{"literal-admin-without-capability", "admin", `{"key":"trigger_rules","value":` + validRule + `,"previous_version":"` + version + `"}`, false, http.StatusForbidden},
		{"relative-workspace", "admin", `{"key":"trigger_rules","value":[{"source":"watch-dir","pipeline":{"template":"build","workspace":"repo"}}],"previous_version":"` + version + `"}`, true, http.StatusBadRequest},
		{"unsupported-source", "admin", `{"key":"trigger_rules","value":[{"source":"webhook","pipeline":{"template":"build","workspace":"/repo"}}],"previous_version":"` + version + `"}`, true, http.StatusBadRequest},
		{"trailing-json", "admin", `{"key":"trigger_rules","value":` + validRule + `,"previous_version":"` + version + `"}{}`, true, http.StatusBadRequest},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodPost, "/v1/workflow/config/set", strings.NewReader(test.body))
			setWorkflowIdentity(req, test.user, test.operator)
			rec := httptest.NewRecorder()
			server.ServeHTTP(rec, req)
			if rec.Code != test.status {
				t.Fatalf("status=%d body=%s, want %d", rec.Code, rec.Body.String(), test.status)
			}
		})
	}
}

func TestWorkflowTriggerRegistryReportsMalformedConfigWithoutHidingIt(t *testing.T) {
	server, _, _ := newTestServer(t)
	configPath := filepath.Join(t.TempDir(), "aimee.yaml")
	bad := "trigger_rules:\n  - source: watch-dir\n    event: ../outside\n    pipeline:\n      template: build\n      workspace: /repo\n"
	if err := os.WriteFile(configPath, []byte(bad), 0o600); err != nil {
		t.Fatal(err)
	}
	configStore, _ := appconfig.NewStore(configPath)
	server.SetConfigStore(configStore)
	req := httptest.NewRequest(http.MethodGet, "/v1/workflow/triggers", nil)
	setWorkflowIdentity(req, "admin", true)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"registry_error"`) ||
		!strings.Contains(rec.Body.String(), "repository-relative") {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	content, _ := os.ReadFile(configPath)
	if string(content) != bad {
		t.Fatalf("malformed config was changed by a read:\n%s", content)
	}
}

func TestProposalTriggerFilesCompleteGitBlobAndDeduplicates(t *testing.T) {
	server, store, artifacts := newTestServer(t)
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	workflow := `name: build
start: plan
nodes:
  - id: source
    block: author.proposal
    next: plan
  - id: plan
    block: author.plan
    in: {proposal: source.out}
`
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), []byte(workflow), 0o600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = workflowDir
	repo := filepath.Join(root, "repo")
	proposalDir := filepath.Join(repo, "docs", "proposals", "pending")
	if err := os.MkdirAll(proposalDir, 0o700); err != nil {
		t.Fatal(err)
	}
	tail := "TRIGGER_PROPOSAL_END"
	proposal := strings.Repeat("trigger proposal body λ\n", 180_000) + tail
	if err := os.WriteFile(filepath.Join(proposalDir, "large.md"), []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("large.md", filepath.Join(proposalDir, "linked.md")); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "init")
	runGit(t, repo, "config", "user.email", "test@example.invalid")
	runGit(t, repo, "config", "user.name", "Test")
	runGit(t, repo, "add", ".")
	runGit(t, repo, "commit", "-m", "proposal")

	body := []byte(`{"source":"proposals","proposal":"large","workspace":` +
		strconv.Quote(repo) + `,"ref":"HEAD","pipeline":"build","mode":"autonomous"}`)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		WorkItemID string `json:"work_item_id"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), response.WorkItemID)
	if err != nil {
		t.Fatal(err)
	}
	if item.State != "active" || item.Stage != "plan" {
		t.Fatalf("filed item=%+v", item)
	}
	got, err := artifacts.Proposal(response.WorkItemID)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != proposal || !strings.HasSuffix(string(got), tail) {
		t.Fatal("triggered proposal artifact was not preserved completely")
	}

	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusConflict || !strings.Contains(rec.Body.String(), "already filed") {
		t.Fatalf("duplicate status=%d body=%s", rec.Code, rec.Body.String())
	}
	if err := os.WriteFile(filepath.Join(repo, "unrelated.txt"), []byte("advance watched ref\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "add", "unrelated.txt")
	runGit(t, repo, "commit", "-m", "advance branch without changing proposal")
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusConflict || !strings.Contains(rec.Body.String(), "already filed") {
		t.Fatalf("moving-ref duplicate status=%d body=%s", rec.Code, rec.Body.String())
	}
	if err := os.WriteFile(filepath.Join(proposalDir, "large.md"), []byte(proposal+"\nchanged blob\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "add", "docs/proposals/pending/large.md")
	runGit(t, repo, "commit", "-m", "change proposal blob")
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("changed blob status=%d body=%s", rec.Code, rec.Body.String())
	}
	linkedBody := []byte(`{"source":"proposals","proposal":"linked","workspace":` +
		strconv.Quote(repo) + `,"ref":"HEAD","pipeline":"build","mode":"autonomous"}`)
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(linkedBody)))
	if rec.Code != http.StatusConflict || !strings.Contains(rec.Body.String(), "not found") {
		t.Fatalf("symlink status=%d body=%s", rec.Code, rec.Body.String())
	}
}

func TestConfiguredTriggerScannerFilesPendingProposalWithoutManualFire(t *testing.T) {
	server, store, artifacts := newTestServer(t)
	root := t.TempDir()
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), []byte("name: build\nstart: draft\nnodes:\n  - id: draft\n    block: author.proposal\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = workflowDir
	server.workflows = nil
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(filepath.Join(repo, "docs/proposals/pending"), 0o700); err != nil {
		t.Fatal(err)
	}
	proposal := "# Automatically discovered\n\ncomplete content\n"
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/auto.md"), []byte(proposal), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/.gitkeep"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "docs/proposals/pending/notes.txt"), []byte("not a proposal"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "init")
	runGit(t, repo, "config", "user.email", "test@example.invalid")
	runGit(t, repo, "config", "user.name", "Test")
	runGit(t, repo, "add", ".")
	runGit(t, repo, "commit", "-m", "proposal")
	configPath := filepath.Join(root, "aimee.yaml")
	configText := "trigger:\n  max_concurrent: 2\ntrigger_rules:\n  - source: watch-dir\n    event: docs/proposals/pending\n    mode: autonomous\n    pipeline:\n      template: build\n      workspace: " + strconv.Quote(repo) + "\n"
	if err := os.WriteFile(configPath, []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	configStore, err := appconfig.NewStore(configPath)
	if err != nil {
		t.Fatal(err)
	}
	server.SetConfigStore(configStore)
	server.ScanTriggers(context.Background())
	items, err := store.WorkItems(context.Background())
	if err != nil || len(items) != 1 {
		t.Fatalf("items=%v err=%v", items, err)
	}
	got, err := artifacts.Proposal(items[0].ID)
	if err != nil || string(got) != proposal {
		t.Fatalf("proposal=%q err=%v", got, err)
	}
	server.ScanTriggers(context.Background())
	items, _ = store.WorkItems(context.Background())
	if len(items) != 1 {
		t.Fatalf("scanner duplicated proposal: %d items", len(items))
	}
}

func TestRefreshScanRefUsesNewRemoteBranchTip(t *testing.T) {
	root := t.TempDir()
	remote := filepath.Join(root, "remote.git")
	workspace := filepath.Join(root, "workspace")
	publisher := filepath.Join(root, "publisher")
	runGit(t, root, "init", "--bare", remote)
	runGit(t, root, "init", workspace)
	runGit(t, workspace, "config", "user.email", "test@example.invalid")
	runGit(t, workspace, "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(workspace, "seed"), []byte("seed"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, workspace, "add", ".")
	runGit(t, workspace, "commit", "-m", "seed")
	runGit(t, workspace, "branch", "-M", "testing")
	runGit(t, workspace, "remote", "add", "origin", remote)
	runGit(t, workspace, "push", "-u", "origin", "testing")
	runGit(t, remote, "symbolic-ref", "HEAD", "refs/heads/testing")
	runGit(t, workspace, "remote", "set-head", "origin", "testing")
	runGit(t, root, "clone", "--branch", "testing", remote, publisher)
	runGit(t, publisher, "config", "user.email", "test@example.invalid")
	runGit(t, publisher, "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(publisher, "new-proposal"), []byte("new"), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, publisher, "add", ".")
	runGit(t, publisher, "commit", "-m", "new proposal")
	runGit(t, publisher, "push", "origin", "testing")

	ref, err := refreshScanRef(t.Context(), workspace, "testing")
	if err != nil {
		t.Fatal(err)
	}
	if ref != "origin/testing" {
		t.Fatalf("ref=%q", ref)
	}
	listing, err := gitOutput(t.Context(), workspace, "ls-tree", "--name-only", ref)
	if err != nil || !strings.Contains(string(listing), "new-proposal") {
		t.Fatalf("listing=%q err=%v", listing, err)
	}
	localListing, err := gitOutput(t.Context(), workspace, "ls-tree", "--name-only", "testing")
	if err != nil || strings.Contains(string(localListing), "new-proposal") {
		t.Fatalf("local branch unexpectedly moved: %q err=%v", localListing, err)
	}
	defaultRef, err := refreshScanRef(t.Context(), workspace, "")
	if err != nil || defaultRef != "origin/testing" {
		t.Fatalf("default ref=%q err=%v", defaultRef, err)
	}
	if _, err := refreshScanRef(t.Context(), workspace, "missing"); err == nil {
		t.Fatal("missing remote branch silently fell back to a local ref")
	}
	runGit(t, publisher, "checkout", "-b", "feature/nested")
	runGit(t, publisher, "push", "origin", "feature/nested")
	nested, err := refreshScanRef(t.Context(), workspace, "feature/nested")
	if err != nil || nested != "origin/feature/nested" {
		t.Fatalf("nested ref=%q err=%v", nested, err)
	}
	runGit(t, workspace, "remote", "add", "upstream", remote)
	qualified, err := refreshScanRef(t.Context(), workspace, "upstream/testing")
	if err != nil || qualified != "upstream/testing" {
		t.Fatalf("qualified ref=%q err=%v", qualified, err)
	}
	if fullCommitID("abc1234") || !fullCommitID(strings.Repeat("aB", 20)) || !fullCommitID(strings.Repeat("aB", 32)) {
		t.Fatal("full commit id classification changed")
	}
}

func TestVisibleMarkdownProposal(t *testing.T) {
	const root = "docs/proposals/pending"
	cases := map[string]struct {
		candidate string
		want      bool
	}{
		"visible":           {root + "/feature.md", true},
		"case insensitive":  {root + "/FEATURE.MD", true},
		"version directory": {root + "/v1.0/notes.md", true},
		"hidden filename":   {root + "/.gitkeep.md", false},
		"hidden directory":  {root + "/.drafts/feature.md", false},
		"hidden well known": {root + "/.well-known/foo.md", false},
		"wrong extension":   {root + "/notes.txt", false},
		"wrong root":        {"docs/other/feature.md", false},
		"parent escape":     {root + "/../feature.md", false},
		"absolute":          {"/" + root + "/feature.md", false},
		"control character": {root + "/bad\nname.md", false},
		"empty":             {"", false},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			if got := isVisibleMarkdownProposal(root, tc.candidate); got != tc.want {
				t.Errorf("isVisibleMarkdownProposal(%q)=%v want=%v", tc.candidate, got, tc.want)
			}
		})
	}
}

func TestRegularTreePathRejectsSymlinks(t *testing.T) {
	if got, ok := regularTreePath("100644 blob deadbeef\tdocs/proposals/pending/ok.md"); !ok || got != "docs/proposals/pending/ok.md" {
		t.Fatalf("regular path=%q ok=%v", got, ok)
	}
	if _, ok := regularTreePath("120000 blob deadbeef\tdocs/proposals/pending/link.md"); ok {
		t.Fatal("symlink was accepted as a proposal")
	}
}

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	command := exec.Command("git", append([]string{"-C", dir}, args...)...)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("git %s: %v: %s", strings.Join(args, " "), err, output)
	}
}

// trigger.max_concurrent = 0 is an operator saying "admit nothing". The store's
// cap sentinel treats <=0 as unlimited (child slices depend on that), so the
// admission paths must refuse a configured 0 explicitly — otherwise "pause
// admission" silently removes the limit instead of applying it.
func TestConfiguredZeroConcurrencyPausesAdmission(t *testing.T) {
	server, _, _ := newTestServer(t)
	configPath := filepath.Join(t.TempDir(), "aimee.yaml")
	if err := os.WriteFile(configPath, []byte("trigger:\n  max_concurrent: 0\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := appconfig.NewStore(configPath)
	if err != nil {
		t.Fatal(err)
	}
	server.SetConfigStore(store)
	if got := store.Int("trigger.max_concurrent", 2); got != 0 {
		t.Fatalf("config did not load max_concurrent=0, got %d", got)
	}
	body := strings.NewReader(`{"proposal_md":"## do a thing\n\nwhy: because","workflow":"build","repo":"/tmp"}`)
	req := httptest.NewRequest(http.MethodPost, "/v1/dev/submit", body)
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, req)
	if rec.Code != http.StatusConflict {
		t.Fatalf("expected 409 paused-admission, got %d: %s", rec.Code, rec.Body.String())
	}
}

func TestManualFileSubmissionPreservesValidatedProposalSource(t *testing.T) {
	server, store, _ := newTestServer(t)
	root := t.TempDir()
	runGit(t, root, "init")
	runGit(t, root, "config", "user.email", "test@example.com")
	runGit(t, root, "config", "user.name", "Test")
	pendingDir := filepath.Join(root, "docs", "proposals", "pending")
	if err := os.MkdirAll(pendingDir, 0o700); err != nil {
		t.Fatal(err)
	}
	source := "# Proposal: source-aware run\n\n- **State:** pending — single slice.\n\nDo the thing.\n"
	approved := "# Proposal: source-aware run\n\n- **State:** approved — run now.\n\nDo the thing.\n"
	if err := os.WriteFile(filepath.Join(pendingDir, "source-aware.md"), []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, root, "add", ".")
	runGit(t, root, "commit", "-m", "proposal")
	workflowDir := filepath.Join(root, "workflows")
	if err := os.MkdirAll(workflowDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(workflowDir, "build.yaml"), []byte("name: build\nstart: draft\nnodes:\n  - id: draft\n    block: author.proposal\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = workflowDir
	server.workflows = nil

	body, err := json.Marshal(map[string]string{
		"proposal_md": approved,
		"workflow":    "build",
		"repo":        root,
		"source_path": "docs/proposals/pending/source-aware.md",
	})
	if err != nil {
		t.Fatal(err)
	}
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/dev/submit", bytes.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
	var response struct {
		WorkItemID string `json:"work_item_id"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	item, err := store.WorkItem(context.Background(), response.WorkItemID)
	if err != nil {
		t.Fatal(err)
	}
	if item.SourcePath != "docs/proposals/pending/source-aware.md" {
		t.Fatalf("source_path=%q", item.SourcePath)
	}

	body, err = json.Marshal(map[string]string{
		"proposal_md": approved + "unrelated mutation\n",
		"workflow":    "build",
		"repo":        root,
		"source_path": "docs/proposals/pending/source-aware.md",
	})
	if err != nil {
		t.Fatal(err)
	}
	rec = httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/dev/submit", bytes.NewReader(body)))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("mismatched source status=%d body=%s", rec.Code, rec.Body.String())
	}
}

package engine

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

func TestHTTPAgentClientReusesDurableRemoteJob(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var launches atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			launches.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 42})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "agent_name": "kimi"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, Store: store, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "do it", WorkItemID: "wi_test", Stage: "impl", ExecutionVersion: "v1"}
	for range 2 {
		result, err := client.Delegate(t.Context(), request)
		if err != nil || result.Response != "complete" {
			t.Fatalf("result=%+v err=%v", result, err)
		}
	}
	if launches.Load() != 1 {
		t.Fatalf("launches=%d, want durable reuse", launches.Load())
	}
}

func TestHTTPAgentClientAcceptsNonemptyPartialResult(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 7})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "partial", "result": "commit already created", "agent_name": "minimax"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", acceptPartial: true})
	if err != nil || result.Response != "commit already created" || result.Agent != "minimax" || !result.Partial {
		t.Fatalf("result=%+v err=%v", result, err)
	}
}

func TestHTTPAgentClientRejectsPartialUnlessBlockOptsIn(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 8})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "partial", "result": "unverified artifact"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Delegate(t.Context(), DelegateRequest{Role: "draft", Persona: "architect", Prompt: "plan"}); err == nil {
		t.Fatal("partial result advanced through a block that did not opt in")
	}
}

func TestHTTPAgentClientReplaysAcceptedPartialIdempotently(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var launches atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			launches.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 9})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "partial", "result": "committed artifact"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, Store: store, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", WorkItemID: "wi", Stage: "impl", ExecutionVersion: "v1", acceptPartial: true}
	for range 2 {
		result, callErr := client.Delegate(t.Context(), request)
		if callErr != nil || result.Response != "committed artifact" || !result.Partial {
			t.Fatalf("result=%+v err=%v", result, callErr)
		}
	}
	if launches.Load() != 1 {
		t.Fatalf("accepted partial launched %d overlapping jobs", launches.Load())
	}
}

func TestHTTPAgentClientExpiresUnassignedPendingJob(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var launches atomic.Int32
	var cancellations atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			launches.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 17})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "pending", "agent_name": ""})
		case "/v1/job/cancel":
			cancellations.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, Store: store,
		PollEvery: time.Millisecond, PendingTimeout: 20 * time.Millisecond,
		CancelUnassigned: func(context.Context, int, string) (bool, error) {
			cancellations.Add(1)
			return true, nil
		}})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_pending", Stage: "gate", ExecutionVersion: "v1"}
	if _, err := client.Delegate(t.Context(), request); err == nil || !errors.Is(err, ErrDelegateUnassignedExpired) || !strings.Contains(err.Error(), "remained unassigned") {
		t.Fatalf("unassigned pending job did not return structured expiry: %v", err)
	}
	if launches.Load() != 1 || cancellations.Load() != 1 {
		t.Fatalf("launches=%d cancellations=%d", launches.Load(), cancellations.Load())
	}
	if _, err := store.DelegateJob(t.Context(), delegateJobKey(request)); err == nil {
		t.Fatal("expired pending job retained its durable mapping")
	}
	if _, err := client.Delegate(t.Context(), request); err == nil {
		t.Fatal("replayed unassigned pending job did not expire")
	}
	if launches.Load() != 2 {
		t.Fatalf("expired mapping replayed old job; launches=%d", launches.Load())
	}
}

func TestHTTPAgentClientDoesNotExpireAssignedJob(t *testing.T) {
	var polls atomic.Int32
	var cancellations atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 18})
		case "/v1/delegate/status":
			switch polls.Add(1) {
			case 1:
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "pending", "agent_name": ""})
			case 2:
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "running", "agent_name": "codex"})
			default:
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "agent_name": "codex", "result": "complete"})
			}
		case "/v1/job/cancel":
			cancellations.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: 2 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review"})
	if err != nil || result.Response != "complete" || result.Agent != "codex" {
		t.Fatalf("result=%+v err=%v", result, err)
	}
	if cancellations.Load() != 0 {
		t.Fatalf("assigned job was cancelled %d times", cancellations.Load())
	}
}

func TestHTTPAgentClientExpiresAfterTransientStatusFailures(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var polls atomic.Int32
	var cancellations atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 19})
		case "/v1/delegate/status":
			if polls.Add(1) == 1 {
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "pending", "agent_name": ""})
				return
			}
			http.Error(w, "temporary", http.StatusServiceUnavailable)
		case "/v1/job/cancel":
			cancellations.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, Store: store,
		PollEvery: time.Millisecond, PendingTimeout: 20 * time.Millisecond,
		CancelUnassigned: func(context.Context, int, string) (bool, error) {
			cancellations.Add(1)
			return true, nil
		}})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_transient", Stage: "gate", ExecutionVersion: "v1"}
	if _, err := client.Delegate(t.Context(), request); err == nil || !strings.Contains(err.Error(), "remained unassigned") {
		t.Fatalf("transient status failures escaped lease: %v", err)
	}
	if cancellations.Load() != 1 {
		t.Fatalf("cancellations=%d, want 1", cancellations.Load())
	}
	if _, err := store.DelegateJob(t.Context(), delegateJobKey(request)); err == nil {
		t.Fatal("expired job retained mapping after transient status failures")
	}
}

func TestHTTPAgentClientRetainsMappingWhenExpiryCancellationFails(t *testing.T) {
	store, err := db1.Open(filepath.Join(t.TempDir(), "db.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/job/cancel" {
			http.Error(w, "unavailable", http.StatusServiceUnavailable)
			return
		}
		http.NotFound(w, r)
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, Store: store,
		CancelUnassigned: func(context.Context, int, string) (bool, error) {
			return false, errors.New("agent job database unavailable")
		}})
	if err != nil {
		t.Fatal(err)
	}
	const key = "durable-key"
	if err := store.SaveDelegateJob(t.Context(), key, 20); err != nil {
		t.Fatal(err)
	}
	if err := client.expireUnassigned(20, key, time.Now()); err == nil || !errors.Is(err, ErrDelegateUnassignedExpired) {
		t.Fatalf("failed cancellation lost structured expiry: %v", err)
	}
	if jobID, err := store.DelegateJob(t.Context(), key); err != nil || jobID != 20 {
		t.Fatalf("mapping after failed cancel: job=%d err=%v", jobID, err)
	}
}

func TestDelegatePendingTimeoutClampsLiveSource(t *testing.T) {
	timeout := time.Second
	client := &HTTPAgentClient{pendingTimeout: func() time.Duration { return timeout }}
	if got := client.delegatePendingTimeout(); got != MinDelegatePendingTimeout {
		t.Fatalf("minimum clamp=%s", got)
	}
	timeout = 2 * time.Hour
	if got := client.delegatePendingTimeout(); got != MaxDelegatePendingTimeout {
		t.Fatalf("maximum clamp=%s", got)
	}
	timeout = 3 * time.Minute
	if got := client.delegatePendingTimeout(); got != timeout {
		t.Fatalf("in-range timeout=%s", got)
	}
}

func TestDelegateJobKeyIncludesCompleteCorrectivePrompt(t *testing.T) {
	base := DelegateRequest{Role: "draft", Persona: "architect", Prompt: "original", WorkItemID: "wi", Stage: "scope", ExecutionVersion: "v1"}
	repair := base
	repair.Prompt += "\ncomplete invalid response"
	if delegateJobKey(base) == delegateJobKey(repair) {
		t.Fatal("corrective synthesis reused the original durable job key")
	}
}

func TestDelegateJobKeyIncludesExplicitEligibilityFallback(t *testing.T) {
	assigned := DelegateRequest{Role: "review", Persona: "qa", Delegate: "kimi", Prompt: "review", WorkItemID: "wi", Stage: "gate", ExecutionVersion: "v1"}
	fallback := assigned
	fallback.Delegate = ""
	fallback.RetryTag = "eligible-fallback:0:kimi"
	if delegateJobKey(assigned) == delegateJobKey(fallback) {
		t.Fatal("eligibility fallback reused the assigned delegate job key")
	}
}

func TestHTTPAgentClientEligibleRosterUsesEnabledRoleCapacity(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/agent/list" {
			http.NotFound(w, r)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"agents": []map[string]any{
			{"name": "codex", "provider": "chatgpt", "enabled": true, "max_parallel": 10, "roles": []string{"all"}},
			{"name": "minimax", "provider": "anthropic", "enabled": true, "max_parallel": 4, "roles": []string{"review"}},
			{"name": "primary", "provider": "claude", "enabled": true, "primary_only": true, "max_parallel": 4, "roles": []string{"all"}},
			{"name": "disabled", "provider": "openai", "enabled": false, "max_parallel": 3, "roles": []string{"all"}},
			{"name": "code-only", "provider": "openai", "enabled": true, "max_parallel": 2, "roles": []string{"code"}},
			{"name": "   ", "provider": "openai", "enabled": true, "max_parallel": 2, "roles": []string{"all"}},
			{"name": "empty-role", "provider": "openai", "enabled": true, "max_parallel": 2, "roles": []string{""}},
		}})
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	roster, err := client.EligibleAgents(context.Background(), "review")
	if err != nil {
		t.Fatal(err)
	}
	if len(roster) != 2 || roster[0].Name != "codex" || roster[0].MaxParallel != 10 || roster[1].Name != "minimax" {
		t.Fatalf("roster=%+v", roster)
	}
	if _, err := client.EligibleAgents(context.Background(), ""); err == nil {
		t.Fatal("empty eligibility role accepted")
	}
}

func TestHTTPAgentClientRequiresAuthenticationOffLoopback(t *testing.T) {
	if _, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: "https://resource-plane.example"}); err == nil {
		t.Fatal("unauthenticated non-loopback agent service accepted")
	}
	if _, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: "https://resource-plane.example", Bearer: "token"}); err != nil {
		t.Fatalf("authenticated remote agent service rejected: %v", err)
	}
}

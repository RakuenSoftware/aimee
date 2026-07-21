package engine

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
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

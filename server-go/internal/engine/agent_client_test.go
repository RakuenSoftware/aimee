package engine

import (
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

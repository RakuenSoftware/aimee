package plane

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestHTTPAgentClientReusesDurableRemoteJob(t *testing.T) {
	plane := newReservationPlane(42)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "agent_name": "kimi"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
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
	if plane.launchCount() != 1 {
		t.Fatalf("launches=%d, want durable reuse", plane.launchCount())
	}
}

func TestHTTPAgentClientDistinguishesRejectedFromAmbiguousDispatch(t *testing.T) {
	request := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement"}
	for _, tc := range []struct {
		name       string
		status     int
		body       string
		dispatched bool
	}{
		{name: "admission rejection", status: http.StatusBadRequest, body: `{"error":"cost limit cannot fit prompt"}`},
		{name: "accepted response lost", status: http.StatusOK, body: `{`, dispatched: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(tc.status)
				_, _ = w.Write([]byte(tc.body))
			}))
			defer server.Close()
			client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
			if err != nil {
				t.Fatal(err)
			}
			_, callErr := client.Delegate(t.Context(), request)
			if callErr == nil {
				t.Fatal("expected delegate failure")
			}
			var execution *DelegateExecutionError
			if errors.As(callErr, &execution) != tc.dispatched {
				t.Fatalf("error=%T %v dispatched=%v", callErr, callErr, execution)
			}
			if execution != nil && (execution.CostKnown || execution.CostUSD != 0) {
				t.Fatalf("ambiguous response claimed measured cost: %+v", execution)
			}
		})
	}
}

func TestHTTPAgentClientRetainsAmbiguityAfterMeasuredReroute(t *testing.T) {
	var launches atomic.Int32
	var secondLimit float64
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			job := launches.Add(1)
			if job == 2 {
				secondLimit, _ = payload["max_cost_usd"].(float64)
				_, _ = w.Write([]byte(`{`))
				return
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": job})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "failed", "error": "quota", "cost_usd": .1})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	_, callErr := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review", MaxCostUSD: .5})
	var execution *DelegateExecutionError
	if !errors.As(callErr, &execution) || !execution.Dispatched || execution.CostKnown || execution.CostUSD != .1 {
		t.Fatalf("error=%v execution=%+v", callErr, execution)
	}
	if launches.Load() != 2 || secondLimit != .4 {
		t.Fatalf("launches=%d second_limit=%v", launches.Load(), secondLimit)
	}
}

func TestHTTPAgentClientReplayMissRemainsUnresolved(t *testing.T) {
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: "http://127.0.0.1"})
	if err != nil {
		t.Fatal(err)
	}
	_, callErr := client.Delegate(t.Context(), DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", ReplayOnly: true})
	var execution *DelegateExecutionError
	if !errors.As(callErr, &execution) || !execution.Dispatched || execution.CostKnown {
		t.Fatalf("error=%v execution=%+v", callErr, execution)
	}
}

func TestHTTPAgentClientDurableSlotsLaunchDistinctJobs(t *testing.T) {
	var launches atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			if provided, ok := payload["provided_target"].(bool); !ok || !provided {
				t.Errorf("provided review target missing from payload %v", payload)
			}
			if limit, ok := payload["max_cost_usd"].(float64); !ok || limit != .5 {
				t.Errorf("workflow cost limit missing from payload %v", payload)
			}
			for _, localOnly := range []string{"durable_slot", "DurableSlot", "durableslot", "retry_tag", "RetryTag"} {
				if _, leaked := payload[localOnly]; leaked {
					t.Errorf("local durable-key field %q leaked in payload %v", localOnly, payload)
				}
			}
			jobID := launches.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": jobID})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "participant": "delegate-job:1"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "qa", Prompt: "review complete artifact",
		WorkItemID: "wi_slots", Stage: "gate", ExecutionVersion: "v1", ProvidedTarget: true, MaxCostUSD: .5}
	for _, slot := range []string{"panel:gate:round:1:seat:0", "panel:gate:round:1:seat:1"} {
		request.DurableSlot = slot
		if _, err := client.Delegate(t.Context(), request); err != nil {
			t.Fatal(err)
		}
	}
	if launches.Load() != 2 {
		t.Fatalf("distinct durable slots launched %d jobs", launches.Load())
	}
}

func TestHTTPAgentClientOmitsProvidedTargetByDefault(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			if _, present := payload["provided_target"]; present {
				t.Errorf("ordinary delegate request claimed a provided target: %v", payload)
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 1})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "participant": "delegate-job:1"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review worktree"}); err != nil {
		t.Fatal(err)
	}
}

func TestHTTPAgentClientForwardsResourceCaps(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			if got := payload["max_turns_cap"]; got != float64(24) {
				t.Errorf("max_turns_cap=%v payload=%v", got, payload)
			}
			if got := payload["tool_loop_timeout_ms_cap"]; got != float64(420000) {
				t.Errorf("tool_loop_timeout_ms_cap=%v payload=%v", got, payload)
			}
			if _, leaked := payload["MaxTurnsCap"]; leaked {
				t.Errorf("Go-local field name leaked in payload: %v", payload)
			}
			if _, leaked := payload["ToolLoopTimeoutMSCap"]; leaked {
				t.Errorf("Go-local timeout field name leaked in payload: %v", payload)
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 1})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "participant": "delegate-job:1"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review artifact", MaxTurnsCap: 24, ToolLoopTimeoutMSCap: 420000}); err != nil {
		t.Fatal(err)
	}
}

func TestHTTPAgentClientGroupDelegatesEverySpecificationWithoutResolvingRandom(t *testing.T) {
	var launches atomic.Int32
	var mu sync.Mutex
	var vias []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agent/list":
			_ = json.NewEncoder(w).Encode(map[string]any{"agents": []map[string]any{
				{"name": "a-down", "provider": "third", "model": "down", "enabled": true, "delegate_available": false, "max_parallel": 10, "roles": []string{"review"}, "personas": []string{"all"}},
				{"name": "codex", "provider": "openai", "model": "gpt", "enabled": true, "max_parallel": 2, "roles": []string{"review"}, "personas": []string{"all"}},
				{"name": "minimax", "provider": "minimax", "model": "m3", "enabled": true, "max_parallel": 2, "roles": []string{"review"}, "personas": []string{"all"}},
			}})
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			mu.Lock()
			vias = append(vias, fmt.Sprint(payload["via"]))
			mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": launches.Add(1), "participant": "opaque-participant"})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	results := client.DelegateGroup(t.Context(), []DelegateRequest{
		{Role: "review", Persona: "security", Prompt: "review artifact"},
		{Role: "review", Persona: "qa", Prompt: "review artifact", Delegate: "codex"},
		{Role: "review", Persona: "architect", Prompt: "review artifact"},
	})
	if len(results) != 3 || launches.Load() != 3 {
		t.Fatalf("results=%d launches=%d", len(results), launches.Load())
	}
	for _, result := range results {
		if result.Err != nil {
			t.Fatal(result.Err)
		}
		if result.Participant != "opaque-participant" {
			t.Fatalf("participant = %q, want delegate-issued continuation", result.Participant)
		}
	}
	mu.Lock()
	defer mu.Unlock()
	sort.Strings(vias)
	if fmt.Sprint(vias) != "[codex codex minimax]" {
		t.Fatalf("delegate group did not fill diverse seats: %v", vias)
	}
}

func TestHTTPAgentClientRetriesUnpinnedRoutingButNeverWeakensPin(t *testing.T) {
	var launches atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": launches.Add(1)})
		case "/v1/delegate/status":
			if launches.Load() == 1 {
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "failed", "error": "subscription temporarily exhausted"})
			} else {
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "complete", "agent_name": "minimax"})
			}
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review artifact"})
	if err != nil || result.Agent != "minimax" || launches.Load() != 2 {
		t.Fatalf("result=%+v err=%v launches=%d", result, err, launches.Load())
	}

	launches.Store(0)
	_, err = client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review artifact", Delegate: "codex"})
	if err == nil || launches.Load() != 1 {
		t.Fatalf("explicit pin was substituted: err=%v launches=%d", err, launches.Load())
	}
}

func TestHTTPAgentClientPreservesContextCancellationWhenRemoteDoesNotAcknowledge(t *testing.T) {
	statusStarted := make(chan struct{})
	statusRelease := make(chan struct{})
	var statusOnce sync.Once
	plane := newReservationPlane(45)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			statusOnce.Do(func() { close(statusStarted) })
			<-statusRelease
		case "/v1/jobs/cancel":
			_ = json.NewEncoder(w).Encode(map[string]any{"cancelled": false})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review complete artifact",
		WorkItemID: "wi_cancel_false", Stage: "gate", ExecutionVersion: "v1"}
	ctx, cancel := context.WithCancel(t.Context())
	done := make(chan error, 1)
	go func() {
		_, callErr := client.Delegate(ctx, request)
		done <- callErr
	}()
	<-statusStarted
	cancel()
	callErr := <-done
	close(statusRelease)
	if !errors.Is(callErr, context.Canceled) {
		t.Fatalf("context cancellation was replaced: %v", callErr)
	}
	// The remote refused to acknowledge the cancellation, so the job may still be
	// running and its reservation must survive for a durable replay.
	if jobID, ok := plane.reservedJob(DelegateJobKey(request)); !ok || jobID != 45 {
		t.Fatalf("unacknowledged remote cancellation released the reservation: job=%d held=%v", jobID, ok)
	}
}

func TestHTTPAgentClientCancelsDelegateResourceAndForgetsAcknowledgedJob(t *testing.T) {
	statusStarted := make(chan struct{})
	statusRelease := make(chan struct{})
	var statusOnce sync.Once
	var singularCancels atomic.Int32
	var pluralCancels atomic.Int32
	var cancelledJobID atomic.Int32
	plane := newReservationPlane(43)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			statusOnce.Do(func() { close(statusStarted) })
			<-statusRelease
		case "/v1/job/cancel":
			singularCancels.Add(1)
			_ = json.NewEncoder(w).Encode(map[string]any{"cancelled": true})
		case "/v1/jobs/cancel":
			pluralCancels.Add(1)
			var body map[string]any
			_ = json.NewDecoder(r.Body).Decode(&body)
			if len(body) != 2 || body["reason"] != "WFE turn cancelled" {
				t.Errorf("cancel body=%v", body)
			}
			jobID, _ := body["job_id"].(float64)
			cancelledJobID.Store(int32(jobID))
			_ = json.NewEncoder(w).Encode(map[string]any{"cancelled": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_cancel", Stage: "gate", ExecutionVersion: "v1"}
	ctx, cancel := context.WithCancel(t.Context())
	done := make(chan error, 1)
	go func() {
		_, callErr := client.Delegate(ctx, request)
		done <- callErr
	}()
	<-statusStarted
	cancel()
	if callErr := <-done; !errors.Is(callErr, context.Canceled) {
		t.Fatalf("delegate cancellation error=%v", callErr)
	}
	close(statusRelease)
	if singularCancels.Load() != 0 || pluralCancels.Load() != 1 || cancelledJobID.Load() != 43 {
		t.Fatalf("singular=%d plural=%d job_id=%d", singularCancels.Load(), pluralCancels.Load(), cancelledJobID.Load())
	}
	if _, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatal("acknowledged cancelled delegate retained its reservation")
	}
}

func TestCancelRemoteRetainsMappingWithoutBooleanAcknowledgement(t *testing.T) {
	tests := []struct {
		name, response string
		status         int
	}{
		{name: "explicit false", response: `{"cancelled":false}`},
		{name: "missing field", response: `{}`},
		{name: "null field", response: `{"cancelled":null}`},
		{name: "numeric field", response: `{"cancelled":1}`},
		{name: "wrong type", response: `{"cancelled":"true"}`},
		{name: "error status with true body", status: http.StatusInternalServerError, response: `{"cancelled":true}`},
		{name: "empty body"},
		{name: "no content", status: http.StatusNoContent},
		{name: "non json", response: `cancelled`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			plane := newReservationPlane(44)
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/v1/delegate/reservation/forget" {
					plane.forget(w, r)
					return
				}
				if r.URL.Path != "/v1/jobs/cancel" {
					http.NotFound(w, r)
					return
				}
				if tc.status != 0 {
					w.WriteHeader(tc.status)
				}
				_, _ = w.Write([]byte(tc.response))
			}))
			defer server.Close()
			client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
			if err != nil {
				t.Fatal(err)
			}
			const key = "cancel-unacknowledged"
			plane.reserve(key, 44)
			cancelErr := client.CancelAndRelease(44, key, t.Context())
			if cancelErr == nil {
				t.Fatal("unacknowledged cancellation returned success")
			}
			if !errors.Is(cancelErr, ErrDelegateCancelUnacknowledged) {
				t.Fatalf("cancel error=%v", cancelErr)
			}
			if jobID, held := plane.reservedJob(key); !held || jobID != 44 {
				t.Fatalf("unacknowledged cancellation released the reservation: job=%d held=%v", jobID, held)
			}
		})
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
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", AcceptPartial: true})
	if err != nil || result.Response != "commit already created" || result.Agent != "minimax" || !result.Partial {
		t.Fatalf("result=%+v err=%v", result, err)
	}
}

func TestHTTPAgentClientRejectsLaunchWithoutJobID(t *testing.T) {
	var launches atomic.Int32
	plane := newReservationPlane(7)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		launches.Add(1)
		if r.URL.Path != "/v1/delegate/run" {
			http.NotFound(w, r)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"error": "no\neligible\tcapacity"})
	}))
	defer server.Close()
	// A WorkItemID is what makes a call replayable: it is the only reason an
	// execution key is sent. Both shapes must report the same launch failure.
	for _, tc := range []struct {
		name    string
		request DelegateRequest
	}{
		{name: "replayable", request: DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review", WorkItemID: "wi_no_capacity", Stage: "gate", ExecutionVersion: "v1"}},
		{name: "without a reservation", request: DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			client, clientErr := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
			if clientErr != nil {
				t.Fatal(clientErr)
			}
			_, callErr := client.Delegate(t.Context(), tc.request)
			if !errors.Is(callErr, ErrDelegateNoJobID) || !strings.Contains(callErr.Error(), `no\x0aeligible\x09capacity`) {
				t.Fatalf("launch error=%v", callErr)
			}
		})
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_no_capacity", Stage: "gate", ExecutionVersion: "v1"}
	// A launch that produced no usable job id must leave nothing reserved:
	// a phantom reservation would make every retry replay a launch that
	// never happened instead of recovering when capacity returns.
	if jobID, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatalf("invalid launch reserved job=%d", jobID)
	}
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	for range 2 {
		if _, retryErr := client.Delegate(t.Context(), request); !errors.Is(retryErr, ErrDelegateNoJobID) {
			t.Fatalf("retry error=%v", retryErr)
		}
	}
	// Two subtests above and two explicit retries must each reach the resource
	// plane; no invalid durable mapping may short-circuit a later attempt.
	if launches.Load() != 4 {
		t.Fatalf("launches=%d, want 4", launches.Load())
	}
	emptyServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{}`))
	}))
	defer emptyServer.Close()
	emptyClient, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: emptyServer.URL})
	if err != nil {
		t.Fatal(err)
	}
	_, err = emptyClient.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review"})
	if !errors.Is(err, ErrDelegateNoJobID) || !strings.Contains(err.Error(), "empty launch response") {
		t.Fatalf("empty launch error=%v", err)
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
	plane := newReservationPlane(9)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "partial", "result": "committed artifact"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", WorkItemID: "wi", Stage: "impl", ExecutionVersion: "v1", AcceptPartial: true}
	for range 2 {
		result, callErr := client.Delegate(t.Context(), request)
		if callErr != nil || result.Response != "committed artifact" || !result.Partial {
			t.Fatalf("result=%+v err=%v", result, callErr)
		}
	}
	if plane.launchCount() != 1 {
		t.Fatalf("accepted partial launched %d overlapping jobs", plane.launchCount())
	}
}

func TestHTTPAgentClientExpiresUnassignedPendingJob(t *testing.T) {
	var launches atomic.Int32
	var cancellations atomic.Int32
	plane := newReservationPlane(17)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			launches.Add(1)
			plane.run(w, r)
		case "/v1/delegate/cancel_unassigned":
			cancelUnassignedHandler(&cancellations, true)(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
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
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: 20 * time.Millisecond})
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
	if _, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatal("expired pending job retained its reservation")
	}
	if _, err := client.Delegate(t.Context(), request); err == nil {
		t.Fatal("replayed unassigned pending job did not expire")
	}
	if launches.Load() != 2 {
		t.Fatalf("expired mapping replayed old job; launches=%d", launches.Load())
	}
}

func TestHTTPAgentClientExpiresRoutedPendingJob(t *testing.T) {
	var cancellations atomic.Int32
	plane := newReservationPlane(23)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/cancel_unassigned":
			cancelUnassignedHandler(&cancellations, true)(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			// Routing records agent_name before a worker takes the lease. A
			// pending row is therefore still unassigned even with this field set.
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "pending", "agent_name": "codex"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: MinDelegatePendingTimeout})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		Delegate: "codex", WorkItemID: "wi_routed_pending", Stage: "gate", ExecutionVersion: "v1"}
	if _, err := client.Delegate(t.Context(), request); err == nil || !errors.Is(err, ErrDelegateUnassignedExpired) {
		t.Fatalf("routed pending job did not expire as unassigned: %v", err)
	}
	if cancellations.Load() != 1 {
		t.Fatalf("cancellations=%d, want 1", cancellations.Load())
	}
	if _, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatal("expired routed pending job retained its reservation")
	}
}

func TestHTTPAgentClientExpiresUnassignedRunningJob(t *testing.T) {
	var launches atomic.Int32
	var cancellations atomic.Int32
	var cancelledJobID atomic.Int64
	var cancelReasonMu sync.Mutex
	var cancelReason string
	plane := newReservationPlane(20)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			launches.Add(1)
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "running", "agent_name": ""})
		case "/v1/delegate/cancel_unassigned":
			// The plane owns agent_jobs, so the expiry is asked for over the wire
			// rather than written directly. Record what was asked.
			var body struct {
				JobID  int    `json:"job_id"`
				Reason string `json:"reason"`
			}
			_ = json.NewDecoder(r.Body).Decode(&body)
			cancellations.Add(1)
			cancelledJobID.Store(int64(body.JobID))
			cancelReasonMu.Lock()
			cancelReason = body.Reason
			cancelReasonMu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok", "cancelled": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: 20 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_running", Stage: "gate", ExecutionVersion: "v1"}
	if _, err := client.Delegate(t.Context(), request); err == nil || !errors.Is(err, ErrDelegateUnassignedExpired) {
		t.Fatalf("unassigned running job did not return structured expiry: %v", err)
	}
	if launches.Load() != 1 || cancellations.Load() != 1 {
		t.Fatalf("launches=%d cancellations=%d", launches.Load(), cancellations.Load())
	}
	cancelReasonMu.Lock()
	gotCancelReason := cancelReason
	cancelReasonMu.Unlock()
	if cancelledJobID.Load() != 20 || gotCancelReason != "unassigned delegate lease expired" {
		t.Fatalf("cancelled job=%d reason=%q", cancelledJobID.Load(), gotCancelReason)
	}
	if _, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatal("expired running job retained its reservation")
	}
}

func TestHTTPAgentClientAssignedObservationClearsUnassignedLease(t *testing.T) {
	var polls atomic.Int32
	var cancellations atomic.Int32
	plane := newReservationPlane(21)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			switch polls.Add(1) {
			case 1:
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "running", "agent_name": ""})
			case 2:
				_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "running", "agent_name": "codex"})
			default:
				// Exceed the minimum lease after assignment. A stale unassigned
				// clock would turn this transient status failure into an expiry.
				time.Sleep(MinDelegatePendingTimeout + 100*time.Millisecond)
				http.Error(w, "temporary", http.StatusServiceUnavailable)
			}
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: MinDelegatePendingTimeout})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_assigned", Stage: "gate", ExecutionVersion: "v1"}
	_, err = client.Delegate(t.Context(), request)
	if err == nil || errors.Is(err, ErrDelegateUnassignedExpired) || !strings.Contains(err.Error(), "503") {
		t.Fatalf("assigned observation did not clear lease: %v", err)
	}
	if cancellations.Load() != 0 {
		t.Fatalf("assigned job was cancelled %d times", cancellations.Load())
	}
	// A transient status failure on an assigned job is not an expiry, so its
	// reservation must survive for the replay that follows.
	if jobID, ok := plane.reservedJob(DelegateJobKey(request)); !ok || jobID != 21 {
		t.Fatalf("assigned job reservation was not retained: job=%d held=%v", jobID, ok)
	}
}

func TestHTTPAgentClientTerminalEmptyAgentDoesNotExpire(t *testing.T) {
	var cancellations atomic.Int32
	plane := newReservationPlane(22)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "failed", "agent_name": ""})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: MinDelegatePendingTimeout})
	if err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "review", Persona: "reviewer", Prompt: "review",
		WorkItemID: "wi_terminal", Stage: "gate", ExecutionVersion: "v1"}
	_, err = client.Delegate(t.Context(), request)
	// A terminal failure is not an unassigned-lease expiry: it is reported as
	// the job's own failure, and route retry may have moved on to a later job.
	if err == nil || errors.Is(err, ErrDelegateUnassignedExpired) || !errors.Is(err, ErrDelegateTerminal) {
		t.Fatalf("empty-agent terminal status took wrong path: %v", err)
	}
	if cancellations.Load() != 0 {
		t.Fatalf("terminal job was cancelled %d times", cancellations.Load())
	}
	// Every terminal attempt released its own reservation, so nothing is left
	// reserved for a replay that could only re-serve a failure.
	if held := plane.reservationCount(); held != 0 {
		t.Fatalf("terminal job retained %d reservations", held)
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
	var polls atomic.Int32
	var cancellations atomic.Int32
	plane := newReservationPlane(19)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			plane.run(w, r)
		case "/v1/delegate/cancel_unassigned":
			cancelUnassignedHandler(&cancellations, true)(w, r)
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
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
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL,
		PollEvery: time.Millisecond, PendingTimeout: 20 * time.Millisecond})
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
	if _, held := plane.reservedJob(DelegateJobKey(request)); held {
		t.Fatal("expired job retained its reservation after transient status failures")
	}
}

func TestHTTPAgentClientRetainsMappingWhenExpiryCancellationFails(t *testing.T) {
	plane := newReservationPlane(20)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/delegate/reservation/forget" {
			plane.forget(w, r)
			return
		}
		if r.URL.Path == "/v1/job/cancel" {
			http.Error(w, "unavailable", http.StatusServiceUnavailable)
			return
		}
		http.NotFound(w, r)
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	const key = "durable-key"
	plane.reserve(key, 20)
	if err := client.expireUnassigned(20, key, time.Now()); err == nil || !errors.Is(err, ErrDelegateUnassignedExpired) {
		t.Fatalf("failed cancellation lost structured expiry: %v", err)
	}
	if jobID, held := plane.reservedJob(key); !held || jobID != 20 {
		t.Fatalf("reservation after failed cancel: job=%d held=%v", jobID, held)
	}
}

func TestHTTPAgentClientRetainsMappingWhenExpiryCancellationRejected(t *testing.T) {
	plane := newReservationPlane(23)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/delegate/reservation/forget":
			plane.forget(w, r)
		case "/v1/delegate/cancel_unassigned":
			// The plane refuses: the job is assigned, terminal, or not yet old
			// enough. Whichever it is, this client may not treat it as reclaimed.
			cancelUnassignedHandler(nil, false)(w, r)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	const key = "durable-rejected-key"
	plane.reserve(key, 23)
	if err := client.expireUnassigned(23, key, time.Now()); err == nil ||
		!errors.Is(err, ErrDelegateUnassignedExpired) || !strings.Contains(err.Error(), "durable reservation retained") {
		t.Fatalf("rejected cancellation lost structured expiry: %v", err)
	}
	if jobID, held := plane.reservedJob(key); !held || jobID != 23 {
		t.Fatalf("reservation after rejected cancel: job=%d held=%v", jobID, held)
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
	if DelegateJobKey(base) == DelegateJobKey(repair) {
		t.Fatal("corrective synthesis reused the original durable job key")
	}
}

func TestDelegateJobKeyIncludesExplicitEligibilityFallback(t *testing.T) {
	assigned := DelegateRequest{Role: "review", Persona: "qa", Delegate: "kimi", Prompt: "review", WorkItemID: "wi", Stage: "gate", ExecutionVersion: "v1"}
	fallback := assigned
	fallback.Delegate = ""
	fallback.RetryTag = "eligible-fallback:0:kimi"
	if DelegateJobKey(assigned) == DelegateJobKey(fallback) {
		t.Fatal("eligibility fallback reused the assigned delegate job key")
	}
}

func TestDelegateJobKeyIgnoresInvocationSafetyControls(t *testing.T) {
	original := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", WorkItemID: "wi", Stage: "impl", ExecutionVersion: "v1", MaxCostUSD: 0.75, ToolLoopTimeoutMSCap: 120_000}
	replay := original
	replay.ReplayOnly = true
	replay.MaxCostUSD = 0.13
	replay.ToolLoopTimeoutMSCap = 45_000
	if DelegateJobKey(original) != DelegateJobKey(replay) {
		t.Fatal("replay controls changed the durable job key")
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

func TestHTTPAgentClientReroutesGroupSeatAfterAdmissionBackpressure(t *testing.T) {
	var mu sync.Mutex
	var vias []string
	var launches atomic.Int32
	transport := agentRoundTripFunc(func(r *http.Request) (*http.Response, error) {
		switch r.URL.Path {
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			via, _ := payload["via"].(string)
			mu.Lock()
			vias = append(vias, via)
			mu.Unlock()
			if launches.Add(1) == 1 {
				return agentHTTPResponse(http.StatusServiceUnavailable, `{"error":"agent at capacity [aimee_err=concurrency_limit]"}`), nil
			}
			return agentHTTPResponse(http.StatusOK, `{"job_id":41,"participant":"seat-41"}`), nil
		case "/v1/delegate/status":
			return agentHTTPResponse(http.StatusOK, `{"job_status":"done","agent_name":"idle","result":"reviewed","cost_known":true}`), nil
		default:
			return agentHTTPResponse(http.StatusNotFound, `{}`), nil
		}
	})
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: "http://127.0.0.1", PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.client.Transport = transport
	result, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Prompt: "review", Delegate: "busy", routeSelected: true})
	if err != nil || result.Response != "reviewed" || result.Participant != "seat-41" {
		t.Fatalf("result=%+v err=%v", result, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(vias) != 2 || vias[0] != "busy" || vias[1] != "" {
		t.Fatalf("capacity retry did not return the group seat to generic routing: %v", vias)
	}
}

func TestHTTPAgentClientDoesNotRerouteOperatorPinAfterAdmissionBackpressure(t *testing.T) {
	var launches atomic.Int32
	transport := agentRoundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path != "/v1/delegate/run" {
			return agentHTTPResponse(http.StatusNotFound, `{}`), nil
		}
		launches.Add(1)
		return agentHTTPResponse(http.StatusServiceUnavailable, `{"error":"agent at capacity [aimee_err=concurrency_limit]"}`), nil
	})
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: "http://127.0.0.1", PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.client.Transport = transport
	_, err = client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "qa", Prompt: "review", Delegate: "operator-pinned"})
	if err == nil || !IsCapacityBackpressure(err) || launches.Load() != 1 {
		t.Fatalf("operator pin was rerouted: launches=%d err=%v", launches.Load(), err)
	}
}

type agentRoundTripFunc func(*http.Request) (*http.Response, error)

func (f agentRoundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

func agentHTTPResponse(status int, body string) *http.Response {
	return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}
}

func TestDelegateRouteRetryPolicy(t *testing.T) {
	capacity := errors.New("agent at capacity [aimee_err=concurrency_limit]")
	terminal := fmt.Errorf("%w: provider failed", ErrDelegateTerminal)
	tests := []struct {
		name    string
		request DelegateRequest
		err     error
		want    bool
	}{
		{name: "group capacity race", request: DelegateRequest{Delegate: "busy", routeSelected: true}, err: capacity, want: true},
		{name: "group terminal failure", request: DelegateRequest{Delegate: "failed", routeSelected: true}, err: terminal, want: true},
		{name: "operator pin", request: DelegateRequest{Delegate: "pinned"}, err: capacity},
		{name: "participant continuation", request: DelegateRequest{Participant: "opaque"}, err: terminal},
		{name: "ordinary transport failure", request: DelegateRequest{}, err: errors.New("connection reset")},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := delegateRouteRetryable(tc.request, tc.err); got != tc.want {
				t.Fatalf("delegateRouteRetryable()=%v, want %v", got, tc.want)
			}
		})
	}
}

// A panel seat must not be given to an agent that is already at max_parallel
// while an idle agent is eligible. The router used to count only the seats it
// was assigning in this group, so an agent saturated by other work still looked
// free: the seat was dispatched, admission refused it with
// aimee_err=concurrency_limit, and a panel needing 2 of 3 seats was reported
// unreachable while another agent sat idle. Observed live on run wi_e51e37cf,
// where every "claude" seat failed instantly with
// "agent 'claude' at concurrency limit (max_parallel=3)".
func TestGroupRoutingSkipsAnAgentAlreadyAtItsConcurrencyLimit(t *testing.T) {
	var mu sync.Mutex
	var vias []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agent/list":
			_ = json.NewEncoder(w).Encode(map[string]any{"agents": []map[string]any{
				// Saturated by work outside this group.
				{"name": "busy", "provider": "p1", "model": "m1", "enabled": true, "max_parallel": 3,
					"active_delegates": 3, "roles": []string{"review"}, "personas": []string{"all"}},
				// Idle and equally eligible.
				{"name": "idle", "provider": "p2", "model": "m2", "enabled": true, "max_parallel": 3,
					"active_delegates": 0, "roles": []string{"review"}, "personas": []string{"all"}},
			}})
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			mu.Lock()
			vias = append(vias, fmt.Sprint(payload["via"]))
			mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 1, "participant": "p"})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "ok"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.DelegateGroup(t.Context(), []DelegateRequest{
		{Role: "review", Persona: "architect", Prompt: "review"},
	})
	mu.Lock()
	defer mu.Unlock()
	if len(vias) != 1 {
		t.Fatalf("expected one dispatch, got %v", vias)
	}
	if vias[0] != "idle" {
		t.Fatalf("seat routed to %q; a saturated agent must not be preferred over an idle one", vias[0])
	}
}

// PREFER, never exclude: when every eligible agent is saturated the seat must
// still be filled. Dispatch then fails as capacity backpressure, which the
// engine retries -- refusing to route instead makes the panel unreachable,
// which it does not recover from.
func TestGroupRoutingStillFillsASeatWhenEveryAgentIsSaturated(t *testing.T) {
	var mu sync.Mutex
	var vias []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agent/list":
			_ = json.NewEncoder(w).Encode(map[string]any{"agents": []map[string]any{
				{"name": "busy-a", "provider": "p1", "model": "m1", "enabled": true, "max_parallel": 2,
					"active_delegates": 2, "roles": []string{"review"}, "personas": []string{"all"}},
				{"name": "busy-b", "provider": "p2", "model": "m2", "enabled": true, "max_parallel": 2,
					"active_delegates": 5, "roles": []string{"review"}, "personas": []string{"all"}},
			}})
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			mu.Lock()
			vias = append(vias, fmt.Sprint(payload["via"]))
			mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 1, "participant": "p"})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "ok"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.DelegateGroup(t.Context(), []DelegateRequest{
		{Role: "review", Persona: "architect", Prompt: "review"},
	})
	mu.Lock()
	defer mu.Unlock()
	if len(vias) != 1 {
		t.Fatalf("a saturated roster must still fill the seat, got dispatches %v", vias)
	}
}

// An agent that reports no occupancy at all (older delegate service) must keep
// its previous behaviour and stay routable -- absent must not read as "busy".
func TestGroupRoutingTreatsUnreportedOccupancyAsRoutable(t *testing.T) {
	var mu sync.Mutex
	var vias []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agent/list":
			_ = json.NewEncoder(w).Encode(map[string]any{"agents": []map[string]any{
				{"name": "unreported", "provider": "p1", "model": "m1", "enabled": true, "max_parallel": 1,
					"roles": []string{"review"}, "personas": []string{"all"}},
			}})
		case "/v1/delegate/run":
			var payload map[string]any
			_ = json.NewDecoder(r.Body).Decode(&payload)
			mu.Lock()
			vias = append(vias, fmt.Sprint(payload["via"]))
			mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 1, "participant": "p"})
		case "/v1/delegate/status":
			_ = json.NewEncoder(w).Encode(map[string]any{"job_status": "done", "result": "ok"})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL, PollEvery: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.DelegateGroup(t.Context(), []DelegateRequest{
		{Role: "review", Persona: "architect", Prompt: "review"},
	})
	mu.Lock()
	defer mu.Unlock()
	if len(vias) != 1 || vias[0] != "unreported" {
		t.Fatalf("unreported occupancy must stay routable, got %v", vias)
	}
}

// reservationPlane models the resource plane's half of delegate replay: the
// execution-key reservation now lives with agent_jobs, not in the Go store, so
// tests that protect replay behaviour must drive it through the plane.
//
// It reproduces exactly what the C server does -- resolve the key, launch only
// on a miss, record the reservation with the job id, and compare-delete on
// release -- so a Go-side regression shows up here rather than only in a
// deployed pair.
type reservationPlane struct {
	mu           sync.Mutex
	reservations map[string]int
	launches     int
	nextJobID    int
}

func newReservationPlane(firstJobID int) *reservationPlane {
	return &reservationPlane{reservations: map[string]int{}, nextJobID: firstJobID}
}

// run serves /v1/delegate/run. It returns the reserved job id when the key is
// already held, and otherwise launches and reserves.
func (p *reservationPlane) run(w http.ResponseWriter, r *http.Request) {
	var body map[string]any
	_ = json.NewDecoder(r.Body).Decode(&body)
	key := r.Header.Get("Idempotency-Key")
	p.mu.Lock()
	defer p.mu.Unlock()
	if key != "" {
		if existing, ok := p.reservations[key]; ok {
			_ = json.NewEncoder(w).Encode(map[string]any{"job_id": existing, "replayed": true})
			return
		}
	}
	if replay, _ := body["replay_only"].(bool); replay {
		_ = json.NewEncoder(w).Encode(map[string]any{"job_id": 0, "error": "no delegate reservation to replay"})
		return
	}
	p.launches++
	jobID := p.nextJobID
	p.nextJobID++
	if key != "" {
		p.reservations[key] = jobID
	}
	_ = json.NewEncoder(w).Encode(map[string]any{"job_id": jobID})
}

// forget serves /v1/delegate/reservation/forget with the same compare-delete
// the C ledger performs: a release naming an older job must not erase the
// reservation a retry has since taken under the same key.
func (p *reservationPlane) forget(w http.ResponseWriter, r *http.Request) {
	var body struct {
		ExecutionKey string `json:"execution_key"`
		JobID        int    `json:"job_id"`
	}
	_ = json.NewDecoder(r.Body).Decode(&body)
	p.mu.Lock()
	defer p.mu.Unlock()
	released := false
	if held, ok := p.reservations[body.ExecutionKey]; ok && (body.JobID == 0 || held == body.JobID) {
		delete(p.reservations, body.ExecutionKey)
		released = true
	}
	_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok", "released": released})
}

// reserve seeds a reservation the way a prior launch would have, for tests that
// exercise release paths without launching first.
func (p *reservationPlane) reserve(key string, jobID int) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.reservations[key] = jobID
}

func (p *reservationPlane) reservedJob(key string) (int, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	jobID, ok := p.reservations[key]
	return jobID, ok
}

func (p *reservationPlane) reservationCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.reservations)
}

func (p *reservationPlane) launchCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.launches
}

// cancelUnassignedHandler serves the expiry transition the plane owns, counting
// what was asked for. The plane decides atomically whether the row is still
// unassigned, so the client only ever learns the answer.
func cancelUnassignedHandler(count *atomic.Int32, cancelled bool) func(http.ResponseWriter, *http.Request) {
	return func(w http.ResponseWriter, r *http.Request) {
		if count != nil {
			count.Add(1)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok", "cancelled": cancelled})
	}
}

package workflows

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"golang.org/x/sys/unix"
)

func controlCall(t *testing.T, mux http.Handler, request ControlRequest) ControlResponse {
	t.Helper()
	encoded, err := json.Marshal(request)
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}
	handler := NewControlHandler(mux)
	body, status := handler(ring.ModuleInvocation{StageID: StageControl}, encoded)
	if status != ring.ModuleStatusOK {
		t.Fatalf("expected OK status, got %v", status)
	}
	var response ControlResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	return response
}

// The stage must carry through exactly what the proxy carried: method, path,
// query, body and the two identity headers.
func TestControlCarriesTheProxyRequestShape(t *testing.T) {
	var gotMethod, gotPath, gotQuery, gotBody, gotPrincipal, gotOperator string
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath = r.Method, r.URL.Path
		gotQuery = r.URL.RawQuery
		gotPrincipal = r.Header.Get("X-Aimee-Webuser")
		gotOperator = r.Header.Get("X-Aimee-Workflow-Operator")
		buf := make([]byte, 64)
		n, _ := r.Body.Read(buf)
		gotBody = string(buf[:n])
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`{"ok":true}`))
	})

	response := controlCall(t, mux, ControlRequest{
		Method:           http.MethodPost,
		Path:             "/v1/workflow/items/abc/gate",
		Query:            "webuser=alice",
		Body:             `{"decision":"approve"}`,
		Principal:        "alice",
		WorkflowOperator: true,
	})

	if gotMethod != http.MethodPost || gotPath != "/v1/workflow/items/abc/gate" {
		t.Fatalf("method/path not carried: %s %s", gotMethod, gotPath)
	}
	if gotQuery != "webuser=alice" {
		t.Fatalf("query not carried: %q", gotQuery)
	}
	if gotBody != `{"decision":"approve"}` {
		t.Fatalf("body not carried: %q", gotBody)
	}
	if gotPrincipal != "alice" || gotOperator != "true" {
		t.Fatalf("identity not carried: principal=%q operator=%q", gotPrincipal, gotOperator)
	}
	if response.Status != http.StatusCreated || response.Body != `{"ok":true}` {
		t.Fatalf("response not carried: %+v", response)
	}
}

// A non-operator must not gain the operator header just by asking.
func TestControlOmitsOperatorHeaderWhenNotOperator(t *testing.T) {
	present := true
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, present = r.Header["X-Aimee-Workflow-Operator"]
		w.WriteHeader(http.StatusOK)
	})
	controlCall(t, mux, ControlRequest{
		Method: http.MethodGet, Path: "/v1/workflow/items", WorkflowOperator: false,
	})
	if present {
		t.Fatal("operator header must be absent for a non-operator")
	}
}

// The proxy answered 405 for anything outside its allow-list.
func TestControlRejectsUnsupportedMethod(t *testing.T) {
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Fatal("handler must not be reached for an unsupported method")
	})
	response := controlCall(t, mux, ControlRequest{
		Method: "PATCH", Path: "/v1/workflow/items",
	})
	if response.Status != http.StatusMethodNotAllowed {
		t.Fatalf("expected 405, got %d", response.Status)
	}
}

// CR/LF in a path, query or principal never named a real route, and the proxy
// rejected it to stop header injection. It stays rejected.
func TestControlRejectsControlCharacters(t *testing.T) {
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Fatal("handler must not be reached for an injected request")
	})
	for name, request := range map[string]ControlRequest{
		"path":      {Method: http.MethodGet, Path: "/v1/workflow\r\nX-Evil: 1"},
		"query":     {Method: http.MethodGet, Path: "/v1/workflow", Query: "a=b\r\nX-Evil: 1"},
		"principal": {Method: http.MethodGet, Path: "/v1/workflow", Principal: "alice\r\nX-Evil: 1"},
		"empty":     {Method: http.MethodGet, Path: ""},
	} {
		t.Run(name, func(t *testing.T) {
			response := controlCall(t, mux, request)
			if response.Status != http.StatusBadRequest {
				t.Fatalf("expected 400, got %d", response.Status)
			}
		})
	}
}

// A malformed target must be answered, not panicked on. httptest.NewRequest
// would have taken the process down here.
func TestControlAnswersMalformedTargetWithoutPanic(t *testing.T) {
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Fatal("handler must not be reached for a malformed target")
	})
	response := controlCall(t, mux, ControlRequest{
		Method: http.MethodGet, Path: "://not a url",
	})
	if response.Status != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", response.Status)
	}
}

// Advance and control share a module and are told apart only by stage id.
func TestControlRejectsTheAdvanceStageID(t *testing.T) {
	handler := NewControlHandler(http.NewServeMux())
	_, status := handler(ring.ModuleInvocation{StageID: StageAdvance}, []byte(`{"method":"GET","path":"/v1/workflow"}`))
	if status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("expected InvalidRequest, got %v", status)
	}
}

func TestControlRejectsUndecodableRequest(t *testing.T) {
	handler := NewControlHandler(http.NewServeMux())
	_, status := handler(ring.ModuleInvocation{StageID: StageControl}, []byte("not json"))
	if status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("expected InvalidRequest, got %v", status)
	}
}

// A handler that writes a body without calling WriteHeader answered 200 over
// HTTP, and must still answer 200 here.
func TestControlDefaultsToOKWhenHandlerOnlyWrites(t *testing.T) {
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"items":[]}`))
	})
	response := controlCall(t, mux, ControlRequest{
		Method: http.MethodGet, Path: "/v1/workflow/items",
	})
	if response.Status != http.StatusOK || response.Body != `{"items":[]}` {
		t.Fatalf("unexpected response: %+v", response)
	}
}

// The kind allocation is fixed by the process contract, not chosen.
func TestControlKindMatchesTheContractAllocation(t *testing.T) {
	const workflowsOrdinal = 20
	if want := uint32(4096 + workflowsOrdinal*256 + int(StageControl)); EventControl != want {
		t.Fatalf("EventControl must be %d, got %d", want, EventControl)
	}
}

// A large body must round-trip rather than be silently truncated.
func TestControlCarriesALargeBody(t *testing.T) {
	large := strings.Repeat("a", 1<<20)
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(large))
	})
	response := controlCall(t, mux, ControlRequest{
		Method: http.MethodGet, Path: "/v1/workflow/items",
	})
	if len(response.Body) != len(large) {
		t.Fatalf("body truncated: got %d want %d", len(response.Body), len(large))
	}
}

// One process holds one principal, so the WFE serves the advance decision as
// well as control. Both stages must route from the same handler.
func TestHandlerRoutesBothStages(t *testing.T) {
	reached := false
	mux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		reached = true
		_, _ = w.Write([]byte(`{"ok":true}`))
	})
	handler := NewHandler(mux)

	control, status := handler(ring.ModuleInvocation{StageID: StageControl},
		[]byte(`{"method":"GET","path":"/v1/workflow/items"}`))
	if status != ring.ModuleStatusOK || !reached {
		t.Fatalf("control stage did not reach the mux: status=%v reached=%v", status, reached)
	}
	var response ControlResponse
	if err := json.Unmarshal(control, &response); err != nil {
		t.Fatalf("decode control response: %v", err)
	}
	if response.Status != http.StatusOK {
		t.Fatalf("unexpected control status %d", response.Status)
	}

	// An advance request must reach the decision, not the mux. A malformed one is
	// rejected by the decision itself, which is proof it was routed there.
	if _, status := handler(ring.ModuleInvocation{StageID: StageAdvance}, []byte("short")); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("advance stage not routed to the decision: %v", status)
	}

	if _, status := handler(ring.ModuleInvocation{StageID: 99}, nil); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("unknown stage must be rejected, got %v", status)
	}
}

func TestControlLifecycleCancellationDrainsActiveMux(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	entered, finished := make(chan struct{}), make(chan struct{})
	fallback := make(chan struct{})
	defer close(fallback)
	handler := NewHandlerContext(ctx, http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		close(entered)
		select {
		case <-r.Context().Done():
		case <-fallback:
		}
	}))
	request, err := json.Marshal(ControlRequest{Method: "POST", Path: "/v1/dev/submit"})
	if err != nil {
		t.Fatal(err)
	}
	var body []byte
	var status ring.ModuleStatus
	go func() { body, status = handler(ring.ModuleInvocation{StageID: StageControl}, request); close(finished) }()
	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("mux never entered")
	}
	cancel()
	select {
	case <-finished:
	case <-time.After(2 * time.Second):
		t.Fatal("lifecycle cancellation did not drain active mux")
	}
	if status != ring.ModuleStatusCancelled || len(body) != 0 {
		t.Fatalf("cancelled mux returned body/status %q/%v", body, status)
	}
}

func TestControlAlreadyCancelledLifecycleDoesNotDispatch(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	handler := NewHandlerContext(ctx, http.HandlerFunc(func(http.ResponseWriter, *http.Request) { t.Fatal("cancelled lifecycle dispatched mux") }))
	body, status := handler(ring.ModuleInvocation{StageID: StageControl}, []byte(`{"method":"GET","path":"/v1/workflow/items"}`))
	if status != ring.ModuleStatusCancelled || len(body) != 0 {
		t.Fatalf("cancelled dispatch returned %q/%v", body, status)
	}
}

func TestControlInvocationDeadlineCancelsActiveMux(t *testing.T) {
	var now unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &now); err != nil {
		t.Fatal(err)
	}
	invocation := ring.ModuleInvocation{StageID: StageControl, DeadlineNS: uint64(now.Nano()) + uint64(500*time.Millisecond)}
	entered, finished, fallback := make(chan struct{}), make(chan struct{}), make(chan struct{})
	defer close(fallback)
	handler := NewControlHandler(http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		close(entered)
		select {
		case <-r.Context().Done():
		case <-fallback:
		}
	}))
	var body []byte
	var status ring.ModuleStatus
	go func() {
		body, status = handler(invocation, []byte(`{"method":"GET","path":"/v1/workflow/items"}`))
		close(finished)
	}()
	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("deadline mux never entered")
	}
	select {
	case <-finished:
	case <-time.After(2 * time.Second):
		t.Fatal("invocation deadline did not cancel active mux")
	}
	if status != ring.ModuleStatusDeadlineExceeded || len(body) != 0 {
		t.Fatalf("expired mux returned %q/%v", body, status)
	}
}

func TestControlExpiredInvocationDoesNotDispatch(t *testing.T) {
	handler := NewControlHandler(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { t.Fatal("expired invocation dispatched mux") }))
	body, status := handler(ring.ModuleInvocation{StageID: StageControl, DeadlineNS: 1}, []byte(`{"method":"GET","path":"/v1/workflow/items"}`))
	if status != ring.ModuleStatusDeadlineExceeded || len(body) != 0 {
		t.Fatalf("expired dispatch returned %q/%v", body, status)
	}
}

// The runtime's real FCancel/atomic-flag behavior is tested in ring. This unit
// seam changes the same predicate after dispatch without exposing bus internals.
func TestControlContextObservesCancellationAfterDispatch(t *testing.T) {
	var cancelled atomic.Bool
	ctx, release := controlRequestContext(context.Background(), time.Hour, cancelled.Load)
	defer release()
	if ctx.Err() != nil {
		t.Fatal("request cancelled before bus flag")
	}
	cancelled.Store(true)
	select {
	case <-ctx.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("in-flight bus cancellation not propagated")
	}
	if ctx.Err() != context.Canceled {
		t.Fatalf("bus cancellation error %v", ctx.Err())
	}
}

func TestControlContextAlwaysBoundsStandaloneWork(t *testing.T) {
	ctx, release := controlRequestContext(context.Background(), (ring.ModuleInvocation{}).Remaining(30*time.Second), func() bool { return false })
	defer release()
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) > 30*time.Second {
		t.Fatal("control request has no host-owned deadline bound")
	}
}

package workflows

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// The workflow control stage carried over the event ring.
//
// StageAdvance is a pure decision with a fixed 492-byte contract, which is why
// it migrated first. The control plane is not that: it is a REST surface over a
// durable store, an artifact store and a running engine, which is why it stayed
// on a bespoke AF_UNIX HTTP proxy (src/server/wfe_http_proxy.c) long after the
// decision stage had moved.
//
// It can move now without relocating the engine: the process that already owns
// the store and the mux serves this stage itself, so the bus replaces the
// private transport rather than the ownership. Correlation, deadlines and
// cancellation become the bus's problem instead of a second socket's.
const (
	// EventControl is workflows' second stage kind. The allocation is not a free
	// choice: the process contract fixes it at 4096 + ordinal*256 + stage, and
	// workflows is ordinal 20, so control is advance's successor rather than a
	// kind taken from the top of the range.
	EventControl uint32 = 9218
	StageControl uint32 = 2

	// EventGateDecide serves the fail-closed roundtable gate ruling. JSON, not
	// the fixed framing StageAdvance uses: a panel is a variable number of
	// verdicts, and it is one call per gate rather than per verdict.
	EventGateDecide uint32 = 9219
	StageGateDecide uint32 = 3

	// EventAutonomousRoute serves the S4 autonomous-parity routing policy: which
	// workflows an autonomous run may auto-select, and the floor everything else
	// is lifted to.
	EventAutonomousRoute uint32 = 9220
	StageAutonomousRoute uint32 = 4
)

// ControlRequest is what the C resource plane used to write as an HTTP head. It
// carries exactly what the proxy carried -- no more, so moving the transport
// cannot quietly widen what the control plane accepts.
type ControlRequest struct {
	Method           string `json:"method"`
	Path             string `json:"path"`
	Query            string `json:"query"`
	Body             string `json:"body"`
	Principal        string `json:"principal"`
	WorkflowOperator bool   `json:"workflow_operator"`
}

// ControlResponse is the status and body the proxy used to parse back out of
// the HTTP response.
type ControlResponse struct {
	Status int    `json:"status"`
	Body   string `json:"body"`
}

// controlMethods mirrors the proxy's allow-list. Anything else was a 405 there
// and stays a 405 here.
var controlMethods = map[string]bool{
	http.MethodGet:    true,
	http.MethodPost:   true,
	http.MethodPut:    true,
	http.MethodDelete: true,
}

// controlRecorder captures a handler's response.
//
// This is deliberately not httptest.ResponseRecorder: httptest is a testing
// package, and its request constructor panics on a malformed target. A module
// process must answer a bad request, not die of one.
type controlRecorder struct {
	status int
	body   strings.Builder
	header http.Header
}

func (r *controlRecorder) Header() http.Header {
	if r.header == nil {
		r.header = http.Header{}
	}
	return r.header
}

func (r *controlRecorder) Write(p []byte) (int, error) {
	if r.status == 0 {
		r.status = http.StatusOK
	}
	return r.body.Write(p)
}

func (r *controlRecorder) WriteHeader(status int) {
	if r.status == 0 {
		r.status = status
	}
}

// NewHandler serves both of the workflows process's stages.
//
// One process holds one principal: the bus denies a live duplicate, and the
// grant pins the single executable that may attach as it. So the program that
// owns the control plane serves the advance decision too, rather than a second
// process claiming the same identity.
func NewHandler(mux http.Handler) ring.ModuleHandler {
	return NewHandlerContext(context.Background(), mux)
}

// NewHandlerContext binds admitted control work to the process lifetime.
func NewHandlerContext(ctx context.Context, mux http.Handler) ring.ModuleHandler {
	control := NewControlHandlerContext(ctx, mux)
	return func(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
		switch invocation.StageID {
		case StageAdvance:
			return Handle(invocation, request)
		case StageControl:
			return control(invocation, request)
		case StageGateDecide:
			return handleGateDecide(invocation, request)
		case StageAutonomousRoute:
			return handleAutonomousRoute(invocation, request)
		default:
			return nil, ring.ModuleStatusInvalidRequest
		}
	}
}

// NewControlHandler adapts the workflow HTTP mux to the module contract.
//
// The mux is passed in rather than constructed here so the stage depends on the
// routing surface alone, not on the store, engine and config that assemble it.
func NewControlHandler(mux http.Handler) ring.ModuleHandler {
	return NewControlHandlerContext(context.Background(), mux)
}

// NewControlHandlerContext propagates host cancellation and the bounded stage
// deadline into synchronous HTTP handlers and their subprocesses.
func NewControlHandlerContext(parent context.Context, mux http.Handler) ring.ModuleHandler {
	return func(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
		if mux == nil || parent == nil {
			return nil, ring.ModuleStatusInternal
		}
		// Advance and control share a module and are told apart only by stage id,
		// so an advance id arriving here is a protocol error, not a control call.
		if invocation.StageID != StageControl {
			return nil, ring.ModuleStatusInvalidRequest
		}
		var decoded ControlRequest
		if err := json.Unmarshal(request, &decoded); err != nil {
			return nil, ring.ModuleStatusInvalidRequest
		}
		if !controlMethods[decoded.Method] {
			return controlError(http.StatusMethodNotAllowed, "unsupported workflow control method")
		}
		// The proxy rejected CR/LF because it built a request head by hand.
		// Nothing is concatenated now, but the same inputs stay rejected: a path
		// or principal carrying CR/LF never named a real route.
		if decoded.Path == "" || containsControlChars(decoded.Path, decoded.Query, decoded.Principal) {
			return controlError(http.StatusBadRequest, "invalid workflow control request")
		}
		requestContext, release := controlRequestContext(parent, invocation.Remaining(30*time.Second), invocation.Cancelled)
		defer release()
		if status := controlCancellationStatus(requestContext, invocation); status != ring.ModuleStatusOK {
			return nil, status
		}

		target := decoded.Path
		if decoded.Query != "" {
			target += "?" + decoded.Query
		}
		// http.NewRequest reports a bad target instead of panicking on it.
		httpRequest, err := http.NewRequestWithContext(requestContext, decoded.Method, target, strings.NewReader(decoded.Body))
		if err != nil {
			return controlError(http.StatusBadRequest, "invalid workflow control target")
		}
		httpRequest.Header.Set("Content-Type", "application/json")
		// The private socket's ownership and mode used to be the authentication
		// boundary; the bus attach is that boundary now. The identity headers stay
		// because the mux reads them.
		if decoded.Principal != "" {
			httpRequest.Header.Set("X-Aimee-Webuser", decoded.Principal)
		}
		if decoded.WorkflowOperator {
			httpRequest.Header.Set("X-Aimee-Workflow-Operator", "true")
		}

		recorder := &controlRecorder{}
		mux.ServeHTTP(recorder, httpRequest)
		if status := controlCancellationStatus(requestContext, invocation); status != ring.ModuleStatusOK {
			return nil, status
		}
		status := recorder.status
		if status == 0 {
			// A handler that wrote nothing at all still answered 200 over HTTP.
			status = http.StatusOK
		}
		return encodeControl(ControlResponse{Status: status, Body: recorder.body.String()})
	}
}

func controlError(status int, message string) ([]byte, ring.ModuleStatus) {
	body, err := json.Marshal(map[string]string{"error": message})
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return encodeControl(ControlResponse{Status: status, Body: string(body)})
}

func encodeControl(response ControlResponse) ([]byte, ring.ModuleStatus) {
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	if uint32(len(encoded)) > ring.ModuleMessageMaxBody {
		return nil, ring.ModuleStatusInternal
	}
	return encoded, ring.ModuleStatusOK
}

func containsControlChars(values ...string) bool {
	for _, value := range values {
		if strings.ContainsAny(value, "\r\n") {
			return true
		}
	}
	return false
}

// The bus exposes cancellation as an atomic observation, not a context. Poll it
// only while this request is active; release joins the watcher before returning.
func controlRequestContext(parent context.Context, remaining time.Duration, cancelled func() bool) (context.Context, func()) {
	ctx, cancel := context.WithTimeout(parent, remaining)
	if cancelled() {
		cancel()
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(5 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if cancelled() {
					cancel()
					return
				}
			}
		}
	}()
	return ctx, func() { cancel(); <-done }
}

func controlCancellationStatus(ctx context.Context, invocation ring.ModuleInvocation) ring.ModuleStatus {
	if ctx.Err() == context.DeadlineExceeded || invocation.Remaining(30*time.Second) == 0 {
		return ring.ModuleStatusDeadlineExceeded
	}
	if ctx.Err() != nil || invocation.Cancelled() {
		return ring.ModuleStatusCancelled
	}
	return ring.ModuleStatusOK
}

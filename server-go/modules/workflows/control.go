package workflows

import (
	"encoding/json"
	"net/http"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// The workflow control stage carried over the event bus.
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
func NewHandler(mux http.Handler) bus.ModuleHandler {
	control := NewControlHandler(mux)
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
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
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
}

// NewControlHandler adapts the workflow HTTP mux to the module contract.
//
// The mux is passed in rather than constructed here so the stage depends on the
// routing surface alone, not on the store, engine and config that assemble it.
func NewControlHandler(mux http.Handler) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if mux == nil {
			return nil, bus.ModuleStatusInternal
		}
		// Advance and control share a module and are told apart only by stage id,
		// so an advance id arriving here is a protocol error, not a control call.
		if invocation.StageID != StageControl {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var decoded ControlRequest
		if err := json.Unmarshal(request, &decoded); err != nil {
			return nil, bus.ModuleStatusInvalidRequest
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
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}

		target := decoded.Path
		if decoded.Query != "" {
			target += "?" + decoded.Query
		}
		// http.NewRequest reports a bad target instead of panicking on it.
		httpRequest, err := http.NewRequest(decoded.Method, target, strings.NewReader(decoded.Body))
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
		status := recorder.status
		if status == 0 {
			// A handler that wrote nothing at all still answered 200 over HTTP.
			status = http.StatusOK
		}
		return encodeControl(ControlResponse{Status: status, Body: recorder.body.String()})
	}
}

func controlError(status int, message string) ([]byte, bus.ModuleStatus) {
	body, err := json.Marshal(map[string]string{"error": message})
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return encodeControl(ControlResponse{Status: status, Body: string(body)})
}

func encodeControl(response ControlResponse) ([]byte, bus.ModuleStatus) {
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}

func containsControlChars(values ...string) bool {
	for _, value := range values {
		if strings.ContainsAny(value, "\r\n") {
			return true
		}
	}
	return false
}

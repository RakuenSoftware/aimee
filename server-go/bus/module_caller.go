package bus

import (
	"context"
	"errors"
	"fmt"
	"time"
)

// Calling a module stage from Go.
//
// The serving half of this contract already exists (RunModuleProcess): a module
// receives ModuleOpInvoke and replies ModuleOpResult over the same bus, and the
// reply is correlated so it reaches whoever asked. That is the whole "tell me
// when you are done" mechanism -- there is nothing extra to build on the module
// side, and nothing it has to know about its caller.
//
// What was missing is only this side. Until now the sole module caller was the
// C daemon that hosts the bus, so any other Go process had to reach a stage
// through that daemon's HTTP API rather than over the bus it was already
// running on.
//
// A caller is deliberately not a second bus: the daemon that hosts the bus
// remains its principal and admits this client under a grant that names the
// kinds it may request.

var (
	// ErrModuleCallDeadline reports that no reply arrived in time. The request
	// is cancelled on the way out so the module stops working on it.
	ErrModuleCallDeadline = errors.New("module call deadline exceeded")
	// ErrModuleCallCancelled reports that the caller's context ended first.
	ErrModuleCallCancelled = errors.New("module call cancelled")
	// ErrModuleCallFailed carries a non-OK status from the module itself.
	ErrModuleCallFailed = errors.New("module call failed")
	// ErrModuleCallCapabilityAbsent reports that no module serves the kind.
	// The host answers a serverless request immediately with a control frame;
	// surfacing it as an error is what keeps "nobody is listening" from looking
	// like "the module is slow" until the deadline expires.
	ErrModuleCallCapabilityAbsent = errors.New("module call capability absent")
	// ErrModuleCallRejected reports that the host refused the request, e.g.
	// because the caller's grant does not cover the kind.
	ErrModuleCallRejected = errors.New("module call rejected by host")
)

// ModuleCallStatusError names the module's own outcome, so a caller can tell an
// invalid request from an unavailable capability without parsing prose.
type ModuleCallStatusError struct{ Status ModuleStatus }

func (e *ModuleCallStatusError) Error() string {
	return fmt.Sprintf("module call failed with status %d", e.Status)
}
func (e *ModuleCallStatusError) Unwrap() error { return ErrModuleCallFailed }

// ModuleCaller issues synchronous stage calls over an attached bus client.
//
// One caller serializes its own calls: a Client has a single reply stream, and
// demultiplexing it across concurrent callers is what the C side got wrong --
// there, one shared client meant a long stage blocked every other call in the
// process, including the callback that stage was waiting on. Give each
// concurrent caller its own ModuleCaller over its own Client.
// callerBus is the narrow surface a caller needs, mirroring the serving side's
// moduleBus so both halves are testable without a live host.
type callerBus interface {
	Poll() (Event, bool, error)
	RequestFragment(kind uint32, correlation uint64, payload []byte, more bool) error
	Cancel(kind uint32, correlation uint64) error
	moduleInlineBudget() uint32
}

type ModuleCaller struct {
	client        callerBus
	correlation   uint64
	pollInterval  time.Duration
	responseLimit uint32
}

// NewModuleCaller borrows an attached client. The client must not be shared
// with another caller for the reasons above.
func NewModuleCaller(client *Client) (*ModuleCaller, error) {
	if client == nil {
		return nil, ErrModuleConfig
	}
	return newModuleCaller(client), nil
}

func newModuleCaller(client callerBus) *ModuleCaller {
	return &ModuleCaller{client: client, pollInterval: 200 * time.Microsecond,
		responseLimit: ModuleMessageMaxBody}
}

// Call invokes one stage and returns its response body.
//
// deadline is the caller's own bound. Exceeding it publishes a cancel for the
// correlation before returning, so a module that is still working learns to
// stop rather than finishing into a reply nobody is waiting for.
func (m *ModuleCaller) Call(ctx context.Context, eventKind, stageID uint32, traceID uint64,
	deadline time.Duration, request []byte) ([]byte, error) {
	if m == nil || m.client == nil {
		return nil, ErrModuleConfig
	}
	if uint32(len(request)) > ModuleMessageMaxBody {
		return nil, ErrModuleConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	m.correlation++
	correlation := m.correlation

	var deadlineNS uint64
	if deadline > 0 {
		deadlineNS = monotonicNowNS() + uint64(deadline)
	}
	if err := m.send(eventKind, stageID, traceID, correlation, deadlineNS, request); err != nil {
		return nil, err
	}

	var expiry time.Time
	if deadline > 0 {
		expiry = time.Now().Add(deadline)
	}
	body, err := m.awaitReply(ctx, eventKind, correlation, expiry)
	if err != nil {
		// Tell the module to stop. Best effort: the caller is already leaving,
		// and a failed cancel must not mask why.
		_ = m.client.Cancel(eventKind, correlation)
		return nil, err
	}
	return body, nil
}

// send fragments the invoke across the client's inline budget, exactly as the
// serving side fragments its reply.
func (m *ModuleCaller) send(eventKind, stageID uint32, traceID, correlation, deadlineNS uint64,
	request []byte) error {
	budget := m.client.moduleInlineBudget()
	if budget <= ModuleMessageHeaderLen {
		return ErrModuleRuntime
	}
	chunk := int(budget) - ModuleMessageHeaderLen
	frame := make([]byte, budget)
	first := true
	for offset := 0; first || offset < len(request); {
		first = false
		part := len(request) - offset
		if part > chunk {
			part = chunk
		}
		more := offset+part < len(request)
		message := ModuleMessage{Operation: ModuleOpInvoke, Status: ModuleStatusOK, StageID: stageID,
			BodyLen: uint32(part), DeadlineNS: deadlineNS, TraceID: traceID}
		written, err := message.Encode(frame)
		if err != nil {
			return err
		}
		copy(frame[written:], request[offset:offset+part])
		if err := m.client.RequestFragment(eventKind, correlation, frame[:written+part], more); err != nil {
			return err
		}
		offset += part
	}
	return nil
}

// awaitReply consumes the bus until this call's correlated result arrives.
//
// Events for other correlations are skipped rather than buffered: a caller owns
// its client, so anything else on it is not this call's business.
func (m *ModuleCaller) awaitReply(ctx context.Context, eventKind uint32, correlation uint64,
	expiry time.Time) ([]byte, error) {
	var body []byte
	for {
		event, ok, err := m.client.Poll()
		if err != nil {
			return nil, err
		}
		if !ok {
			if err := ctx.Err(); err != nil {
				return nil, errors.Join(ErrModuleCallCancelled, err)
			}
			if !expiry.IsZero() && time.Now().After(expiry) {
				return nil, ErrModuleCallDeadline
			}
			time.Sleep(m.pollInterval)
			continue
		}
		// The host answers on its own reserved kinds, not the requested one, so
		// a control frame for this correlation has to be matched before the
		// kind filter below or it reads as someone else's traffic and the call
		// waits out a deadline for a reply that was already refused.
		if event.Frame.CorrelationID == correlation {
			switch event.Frame.EventKind {
			case KindCapabilityAbsent:
				return nil, ErrModuleCallCapabilityAbsent
			case KindError:
				return nil, ErrModuleCallRejected
			}
		}
		if event.Frame.EventKind != eventKind || event.Frame.CorrelationID != correlation {
			continue
		}
		message, err := DecodeModuleMessage(event.Payload)
		if err != nil || message.Operation != ModuleOpResult {
			continue
		}
		if message.Status != ModuleStatusOK {
			return nil, &ModuleCallStatusError{Status: message.Status}
		}
		start := ModuleMessageHeaderLen
		end := start + int(message.BodyLen)
		if end > len(event.Payload) {
			return nil, ErrModuleRuntime
		}
		if uint32(len(body))+message.BodyLen > m.responseLimit {
			return nil, ErrModuleRuntime
		}
		body = append(body, event.Payload[start:end]...)
		if event.Frame.HdrFlags&FMore != 0 {
			continue
		}
		return body, nil
	}
}

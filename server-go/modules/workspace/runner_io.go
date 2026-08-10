package workspace

import (
	"encoding/binary"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// The request/response handoff between a turn that needs work done in a tree and
// the client that holds that tree.
//
// The server side submits an op and waits for its result; the client polls for
// the next op, runs it locally, and posts the result back. Both are ordinary
// module calls: the bus runs handlers concurrently and owns cancellation and
// deadlines, so a handler can park on the handoff and the transport decides how
// long that is allowed to last.
//
// One op at a time per tree. A second submitter waits for the current cycle to
// finish rather than interleaving with it, which is what keeps a sequence of git
// commands against one checkout in order.

const (
	StageRunnerIO uint32 = 3

	ioRequestMagic  uint32 = 0x4f495257 /* "WRIO" */
	ioResponseMagic uint32 = 0x52495257 /* "WRIR" */
	ioHeaderLen            = 12
	ioRespHeaderLen        = 8
	// A payload is a marshalled file/exec op, bounded by what the bus will carry
	// in one message.
	ioPayloadMax = 1 << 20

	IOOpSubmit  byte = 1
	IOOpPoll    byte = 2
	IOOpRespond byte = 3

	// How often a parked handler re-checks for cancellation. The bus exposes
	// cancellation as a flag to poll rather than a channel to select on, so the
	// wait is chopped into intervals short enough to stay responsive and long
	// enough not to spin.
	ioWaitTick = 20 * time.Millisecond
)

// exchange is one op in flight: the payload going out, and where its result
// comes back.
type exchange struct {
	payload []byte
	resp    chan []byte
}

type rendezvous struct {
	// Unbuffered on purpose: a submit is not accepted until a poller actually
	// claims it, so an op is never left sitting in a queue nobody is draining.
	reqs chan *exchange

	mu      sync.Mutex
	pending *exchange // claimed by a poller, awaiting its response
	closed  bool
	done    chan struct{}
}

func newRendezvous() *rendezvous {
	return &rendezvous{reqs: make(chan *exchange), done: make(chan struct{})}
}

func (r *rendezvous) close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return
	}
	r.closed = true
	// Fail the claimed op too, so its submitter stops waiting on a client that
	// is no longer there.
	if r.pending != nil {
		close(r.pending.resp)
		r.pending = nil
	}
	close(r.done)
}

func (r *rendezvous) isClosed() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.closed
}

// submit hands one op to whichever client is serving this tree and waits for its
// result. Reports ok=false when the invocation is cancelled or the client goes
// away mid-op — never a partial or invented answer.
func (r *rendezvous) submit(invocation bus.ModuleInvocation, payload []byte) ([]byte, bool) {
	item := &exchange{payload: payload, resp: make(chan []byte, 1)}

	for {
		if invocation.Cancelled() || r.isClosed() {
			return nil, false
		}
		select {
		case r.reqs <- item:
			goto claimed
		case <-r.done:
			return nil, false
		case <-time.After(ioWaitTick):
		}
	}

claimed:
	for {
		if invocation.Cancelled() {
			return nil, false
		}
		select {
		case result, ok := <-item.resp:
			if !ok {
				return nil, false // closed out from under us
			}
			return result, true
		case <-time.After(ioWaitTick):
		}
	}
}

// poll blocks for the next op this client should run. ok=false means the wait
// ended without one — the invocation elapsed, or the tree stopped being served.
// An elapsed poll is the ordinary idle case and the client simply polls again;
// it reports cancelled rather than OK-with-nothing because cancellation
// precedence is the bus's convention, not this module's to reinterpret.
func (r *rendezvous) poll(invocation bus.ModuleInvocation) ([]byte, bool) {
	for {
		if r.isClosed() {
			return nil, false
		}
		if invocation.Cancelled() {
			return nil, false
		}
		select {
		case item := <-r.reqs:
			r.mu.Lock()
			if r.closed {
				r.mu.Unlock()
				return nil, false
			}
			r.pending = item
			r.mu.Unlock()
			return item.payload, true
		case <-r.done:
			return nil, false
		case <-time.After(ioWaitTick):
		}
	}
}

// respond hands a result back to the waiting submitter.
func (r *rendezvous) respond(payload []byte) bool {
	r.mu.Lock()
	item := r.pending
	r.pending = nil
	closed := r.closed
	r.mu.Unlock()
	if closed || item == nil {
		return false // nothing was claimed: a response nobody asked for
	}
	item.resp <- payload
	return true
}

func ioResponse(payload []byte) []byte {
	response := make([]byte, ioRespHeaderLen+len(payload))
	binary.LittleEndian.PutUint32(response[0:4], ioResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(payload)))
	copy(response[ioRespHeaderLen:], payload)
	return response
}

func handleRunnerIO(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < ioHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != ioRequestMagic || request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	op := request[5]
	idLen := int(binary.LittleEndian.Uint16(request[6:8]))
	payloadLen := int(binary.LittleEndian.Uint32(request[8:12]))
	if idLen == 0 || idLen > runnerIDMax || payloadLen > ioPayloadMax ||
		len(request) != ioHeaderLen+idLen+payloadLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if op != IOOpSubmit && op != IOOpPoll && op != IOOpRespond {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	id := string(request[ioHeaderLen : ioHeaderLen+idLen])
	payload := request[ioHeaderLen+idLen:]

	point := runnerFor(id)
	if point == nil {
		// Nobody is serving this tree. Say so rather than parking the caller on a
		// rendezvous that will never be drained.
		return nil, bus.ModuleStatusInvalidRequest
	}

	switch op {
	case IOOpSubmit:
		result, ok := point.submit(invocation, payload)
		if !ok {
			return nil, bus.ModuleStatusCancelled
		}
		return ioResponse(result), bus.ModuleStatusOK
	case IOOpPoll:
		next, ok := point.poll(invocation)
		if !ok {
			return nil, bus.ModuleStatusCancelled
		}
		return ioResponse(next), bus.ModuleStatusOK
	default: // IOOpRespond
		if !point.respond(payload) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		return ioResponse(nil), bus.ModuleStatusOK
	}
}

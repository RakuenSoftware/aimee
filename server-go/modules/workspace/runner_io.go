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
	ioRespHeaderLen        = 12

	// ioMore says another chunk of this result is still coming, so the caller
	// must drain again. One response per request is the protocol, so a stream is
	// pulled rather than pushed.
	ioMore uint32 = 1
	// A payload is a marshalled file/exec op, bounded by what the bus will carry
	// in one message.
	ioPayloadMax = 1 << 20

	IOOpSubmit  byte = 1
	IOOpPoll    byte = 2
	IOOpRespond byte = 3
	// A chunk of a result that is not finished yet. The client knows which of
	// its responses is the last one; the module does not read the payload, so it
	// is told rather than inferring it.
	IOOpRespondPartial byte = 4
	// Pull the next chunk of a submit that is still streaming.
	IOOpDrain byte = 5

	// How often a parked handler re-checks for cancellation. The bus exposes
	// cancellation as a flag to poll rather than a channel to select on, so the
	// wait is chopped into intervals short enough to stay responsive and long
	// enough not to spin.
	ioWaitTick = 20 * time.Millisecond
)

// chunk is one piece of a result. A non-final chunk means the caller must drain
// again for the rest.
type chunk struct {
	payload []byte
	final   bool
}

// exchange is one op in flight: the payload going out, and where its result
// comes back. Results arrive as a stream of chunks; an ordinary op is simply a
// stream of one.
type exchange struct {
	payload []byte
	// Buffered so a client posting chunks is never blocked by how fast the
	// submitter drains them. A stalled reader bounds the producer rather than
	// deadlocking it.
	chunks chan chunk
}

type rendezvous struct {
	// Unbuffered on purpose: a submit is not accepted until a poller actually
	// claims it, so an op is never left sitting in a queue nobody is draining.
	reqs chan *exchange

	mu      sync.Mutex
	pending *exchange // claimed by a poller, awaiting its response
	// The submit whose result is still being drained. Single-in-flight per tree
	// makes this unambiguous: there is only ever one stream to pull from.
	inflight *exchange
	closed   bool
	done     chan struct{}
}

const ioChunkBuffer = 64

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
	// Fail the claimed op and any stream still being drained, so their submitters
	// stop waiting on a client that is no longer there.
	if r.pending != nil {
		close(r.pending.chunks)
		r.pending = nil
	}
	if r.inflight != nil && r.inflight != r.pending {
		r.inflight = nil
	}
	close(r.done)
}

func (r *rendezvous) isClosed() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.closed
}

// submit hands one op to whichever client is serving this tree and waits for the
// first chunk of its result. `more` says the result is a stream and the caller
// must drain for the rest. Reports ok=false when the invocation is cancelled or
// the client goes away mid-op — never a partial passed off as complete.
func (r *rendezvous) submit(invocation bus.ModuleInvocation, payload []byte) (
	result []byte, more bool, ok bool) {
	item := &exchange{payload: payload, chunks: make(chan chunk, ioChunkBuffer)}

	for {
		if invocation.Cancelled() || r.isClosed() {
			return nil, false, false
		}
		select {
		case r.reqs <- item:
			goto claimed
		case <-r.done:
			return nil, false, false
		case <-time.After(ioWaitTick):
		}
	}

claimed:
	r.mu.Lock()
	r.inflight = item
	r.mu.Unlock()
	return r.take(invocation, item)
}

// drain pulls the next chunk of the submit currently streaming on this tree.
func (r *rendezvous) drain(invocation bus.ModuleInvocation) ([]byte, bool, bool) {
	r.mu.Lock()
	item := r.inflight
	r.mu.Unlock()
	if item == nil {
		return nil, false, false // nothing is streaming: a drain nobody asked for
	}
	return r.take(invocation, item)
}

// take waits for one chunk, clearing the in-flight stream once the last arrives.
func (r *rendezvous) take(invocation bus.ModuleInvocation, item *exchange) ([]byte, bool, bool) {
	for {
		if invocation.Cancelled() {
			return nil, false, false
		}
		select {
		case next, open := <-item.chunks:
			if !open {
				return nil, false, false // closed out from under us
			}
			if next.final {
				r.mu.Lock()
				if r.inflight == item {
					r.inflight = nil
				}
				r.mu.Unlock()
			}
			return next.payload, !next.final, true
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

// respond hands one chunk of a result back to the waiting submitter. `final`
// releases the claim; a partial keeps it, because the client is still producing.
func (r *rendezvous) respond(payload []byte, final bool) bool {
	r.mu.Lock()
	item := r.pending
	if final {
		r.pending = nil
	}
	closed := r.closed
	r.mu.Unlock()
	if closed || item == nil {
		return false // nothing was claimed: a response nobody asked for
	}
	select {
	case item.chunks <- chunk{payload: payload, final: final}:
		return true
	default:
		// The submitter is not keeping up and the buffer is full. Refuse rather
		// than block the client here: blocking would hold the claim open and
		// stall the tree for every later op.
		return false
	}
}

func ioResponse(payload []byte, more bool) []byte {
	response := make([]byte, ioRespHeaderLen+len(payload))
	binary.LittleEndian.PutUint32(response[0:4], ioResponseMagic)
	if more {
		binary.LittleEndian.PutUint32(response[4:8], ioMore)
	}
	binary.LittleEndian.PutUint32(response[8:12], uint32(len(payload)))
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
	switch op {
	case IOOpSubmit, IOOpPoll, IOOpRespond, IOOpRespondPartial, IOOpDrain:
	default:
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
		//
		// CAPABILITY_ABSENT, not INVALID_REQUEST. The request was perfectly well
		// formed; what is missing is a runner. A caller cannot tell a malformed
		// request from an unserved tree if both arrive as INVALID_REQUEST, and
		// that mattered: the poll path treated "unserved" as "nothing pending
		// yet" and re-polled immediately, forever, because the wait that paces
		// the loop only happens once a rendezvous exists. Naming the condition
		// is what lets the caller stop, back off, or say so.
		return nil, bus.ModuleStatusCapabilityAbsent
	}

	switch op {
	case IOOpSubmit:
		result, more, ok := point.submit(invocation, payload)
		if !ok {
			return nil, bus.ModuleStatusCancelled
		}
		return ioResponse(result, more), bus.ModuleStatusOK
	case IOOpDrain:
		next, more, ok := point.drain(invocation)
		if !ok {
			return nil, bus.ModuleStatusCancelled
		}
		return ioResponse(next, more), bus.ModuleStatusOK
	case IOOpPoll:
		next, ok := point.poll(invocation)
		if !ok {
			return nil, bus.ModuleStatusCancelled
		}
		return ioResponse(next, false), bus.ModuleStatusOK
	default: // IOOpRespond / IOOpRespondPartial
		if !point.respond(payload, op == IOOpRespond) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		return ioResponse(nil, false), bus.ModuleStatusOK
	}
}

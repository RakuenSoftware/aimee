package db3

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// SearchCaller sends DB3 searches over an attached bus client.
//
// The counterpart to RunProvider, and it belongs beside it for the same reason:
// speaking this wire is a property of the CONTRACT, not of whichever module
// happens to speak it. DB2 routes its own portable searches and the postgres
// module routes the vector operations that reach it, and both are the same
// conversation. Two implementations of it would be two sets of fragmentation
// rules, two correlation schemes, and two answers to what a malformed reply
// means.
//
// It does NOT decide whether to route, which provider to ask, or what to do
// when a provider fails. Those are the caller's policy. This is the wire.

type SearchCaller struct {
	client   callerWireClient
	next     atomic.Uint64
	pollWait time.Duration

	mu      sync.Mutex
	pending map[uint64]*pendingSearch
	closed  chan struct{}
	once    sync.Once
}

// callerWireClient is the part of bus.Client this needs, so the caller is
// testable without standing a bus up.
//
// No Poll. The caller does not read the bus: whoever owns the attachment does,
// and feeds this what it sees. A module has ONE attachment and therefore one
// reader, and a caller that polled alongside it would take half the events.
type callerWireClient interface {
	RequestFragment(kind uint32, correlation uint64, payload []byte, more bool) error
	Cancel(kind uint32, correlation uint64) error
	InlineBudget() uint32
}

type pendingSearch struct {
	body   []byte
	result chan searchOutcome
}

type searchOutcome struct {
	reply   SearchReply
	failure SearchFailure
	err     error
}

// ErrCallerClosed reports a caller whose bus has gone.
var ErrCallerClosed = errors.New("db3: search caller is closed")

// ErrCallerConfig reports a caller that cannot be built or used as asked.
var ErrCallerConfig = errors.New("db3: invalid search caller")

// maxCallerPending bounds outstanding searches.
//
// A ceiling rather than unbounded growth: a provider that stops replying would
// otherwise let every caller's request accumulate until the process died, and
// the failure would be memory rather than the timeouts that would say what is
// actually wrong.
const maxCallerPending = 512

// NewSearchCaller builds a caller on an already-attached client.
//
// It starts no goroutine and reads nothing. Whoever owns the attachment polls
// and hands events to Absorb -- one attachment, one reader.
func NewSearchCaller(client *bus.Client) (*SearchCaller, error) {
	if client == nil {
		return nil, ErrCallerConfig
	}
	return newSearchCaller(client)
}

func newSearchCaller(client callerWireClient) (*SearchCaller, error) {
	if client == nil || client.InlineBudget() == 0 {
		return nil, ErrCallerConfig
	}
	return &SearchCaller{
		client:   client,
		pollWait: time.Millisecond,
		pending:  map[uint64]*pendingSearch{},
		closed:   make(chan struct{}),
	}, nil
}

// Close stops the caller and fails every outstanding search.
//
// Failing them rather than leaving them to time out: a caller that has gone is
// a fact its callers can act on now, and a search that hangs to its deadline
// after the bus is gone spends the deadline saying nothing.
func (c *SearchCaller) Close() {
	c.once.Do(func() {
		close(c.closed)
		c.mu.Lock()
		for id, pending := range c.pending {
			delete(c.pending, id)
			select {
			case pending.result <- searchOutcome{err: ErrCallerClosed}:
			default:
			}
		}
		c.mu.Unlock()
	})
}

// Absorb takes one event from whoever owns the attachment.
//
// Everything not addressed to this caller is ignored, so the owner may offer it
// every event it does not serve itself rather than having to classify first.
func (c *SearchCaller) Absorb(event bus.Event) {
	if event.Frame.EventKind != EventSearch || event.Frame.HdrFlags&bus.FReply == 0 {
		return
	}
	id := event.Frame.CorrelationID

	c.mu.Lock()
	pending, known := c.pending[id]
	if !known {
		c.mu.Unlock()
		return
	}
	// A reply larger than the wire's maximum is a malformed provider, not a
	// large answer. Dropping the assembly rather than growing to fit it keeps a
	// provider from being able to spend the caller's memory.
	if uint64(len(pending.body))+uint64(len(event.Payload)) > uint64(bus.MaxPayload) {
		delete(c.pending, id)
		c.mu.Unlock()
		pending.result <- searchOutcome{err: ErrMalformed}
		return
	}
	pending.body = append(pending.body, event.Payload...)
	if event.Frame.HdrFlags&bus.FMore != 0 {
		c.mu.Unlock()
		return
	}
	body := pending.body
	delete(c.pending, id)
	c.mu.Unlock()

	// A provider answers with a reply OR a failure on the same kind, so which
	// one arrived is decided by which decodes. A body that is neither is a
	// malformed provider and must not be reported as an empty result: empty is
	// indistinguishable from a corpus with no matches.
	if reply, err := DecodeSearchReply(body); err == nil {
		pending.result <- searchOutcome{reply: reply}
		return
	}
	if failure, err := DecodeSearchFailure(body); err == nil {
		pending.result <- searchOutcome{failure: failure}
		return
	}
	pending.result <- searchOutcome{err: ErrMalformed}
}

// Search sends one search and waits for the provider's answer.
//
// Returns the reply, or the provider's typed failure, or an error. Exactly one
// of the first two is meaningful, and a zero SearchFailure.Code means the reply
// is the answer.
func (c *SearchCaller) Search(ctx context.Context,
	request SearchRequest) (SearchReply, SearchFailure, error) {
	if c == nil {
		return SearchReply{}, SearchFailure{}, ErrCallerConfig
	}
	if request.Validate() != nil {
		return SearchReply{}, SearchFailure{}, ErrMalformed
	}
	if ctx == nil {
		ctx = context.Background()
	}
	wire, err := EncodeSearchRequest(request)
	if err != nil {
		return SearchReply{}, SearchFailure{}, err
	}

	id := c.next.Add(1)
	result := make(chan searchOutcome, 1)
	c.mu.Lock()
	select {
	case <-c.closed:
		c.mu.Unlock()
		return SearchReply{}, SearchFailure{}, ErrCallerClosed
	default:
	}
	if len(c.pending) >= maxCallerPending {
		c.mu.Unlock()
		return SearchReply{}, SearchFailure{}, ErrCallerClosed
	}
	c.pending[id] = &pendingSearch{result: result}
	c.mu.Unlock()

	if err := c.send(ctx, id, wire); err != nil {
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		return SearchReply{}, SearchFailure{}, err
	}

	select {
	case outcome := <-result:
		return outcome.reply, outcome.failure, outcome.err
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		// Tell the provider to stop: it is holding a store's resources for an
		// answer nobody is waiting for.
		_ = c.client.Cancel(EventSearch, id)
		return SearchReply{}, SearchFailure{}, ctx.Err()
	case <-c.closed:
		return SearchReply{}, SearchFailure{}, ErrCallerClosed
	}
}

// send fragments the request to the client's inline budget.
func (c *SearchCaller) send(ctx context.Context, id uint64, payload []byte) error {
	budget := int(c.client.InlineBudget())
	if budget > int(bus.MaxPayload) {
		budget = int(bus.MaxPayload)
	}
	if budget <= 0 {
		return ErrCallerConfig
	}
	first := true
	for offset := 0; first || offset < len(payload); {
		first = false
		part := len(payload) - offset
		if part > budget {
			part = budget
		}
		more := offset+part < len(payload)
		for {
			err := c.client.RequestFragment(EventSearch, id, payload[offset:offset+part], more)
			if err == nil {
				break
			}
			// A full ring is back-pressure, not a failure. Everything else is.
			if !errors.Is(err, bus.ErrWouldBlock) {
				return err
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-c.closed:
				return ErrCallerClosed
			case <-time.After(c.pollWait):
			}
		}
		offset += part
	}
	return nil
}

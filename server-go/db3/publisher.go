package db3

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// ApplyPublisher fans one committed operation out to every admitted provider.
//
// The write half of the wire, and it lives here for the same reason the search
// caller does: speaking DB3 is a property of the contract, not of whichever
// module speaks it. A provider is useless without this -- a store that is
// searched but never written to answers correctly and emptily forever, which
// reads as a corpus with no matches rather than a corpus nobody filled.
//
// Publishing rather than addressing a provider: an apply goes to every provider
// that subscribed, because they each keep their own copy and each must see the
// same operations. Which of them a SEARCH goes to is a separate decision.
type ApplyPublisher struct {
	client   publisherWireClient
	pollWait time.Duration

	mu     sync.Mutex
	closed bool
}

// publisherWireClient is the part of bus.Client this needs.
//
// Publish only. Unlike the search caller this expects no reply -- an applied
// acknowledgement comes back as a separate publish the owner routes wherever it
// tracks watermarks.
type publisherWireClient interface {
	Publish(kind uint32, payload []byte) error
	InlineBudget() uint32
}

// ErrPublisherConfig reports a publisher that cannot be built or used.
var ErrPublisherConfig = errors.New("db3: invalid apply publisher")

// NewApplyPublisher builds a publisher on an already-attached client.
func NewApplyPublisher(client *bus.Client) (*ApplyPublisher, error) {
	if client == nil {
		return nil, ErrPublisherConfig
	}
	return newApplyPublisher(client)
}

func newApplyPublisher(client publisherWireClient) (*ApplyPublisher, error) {
	if client == nil || client.InlineBudget() == 0 {
		return nil, ErrPublisherConfig
	}
	return &ApplyPublisher{client: client, pollWait: time.Millisecond}, nil
}

// Close stops the publisher.
func (p *ApplyPublisher) Close() {
	if p == nil {
		return
	}
	p.mu.Lock()
	p.closed = true
	p.mu.Unlock()
}

// PublishApply sends one committed operation to every admitted provider.
//
// A small operation goes as a single frame. A larger one is split into
// chunks, because F_MORE is request/reply only and a publish cannot use it --
// so the envelope carries the offsets instead.
func (p *ApplyPublisher) PublishApply(ctx context.Context, apply Apply) error {
	if p == nil {
		return ErrPublisherConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	wire, err := EncodeApply(apply)
	if err != nil {
		return err
	}
	budget := int(p.client.InlineBudget())
	if budget > int(bus.MaxPayload) {
		budget = int(bus.MaxPayload)
	}
	if budget <= 0 {
		return ErrPublisherConfig
	}

	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return ErrPublisherConfig
	}

	if len(wire) <= budget {
		return p.publish(ctx, EventApply, wire)
	}
	capacity := budget - ApplyChunkHeader
	if capacity <= 0 {
		return bus.ErrPayload
	}
	for offset := 0; offset < len(wire); offset += capacity {
		end := offset + capacity
		if end > len(wire) {
			end = len(wire)
		}
		chunk, err := EncodeApplyChunk(ApplyChunk{
			OperationID: apply.OperationID, Total: uint32(len(wire)),
			Offset: uint32(offset), Data: wire[offset:end],
		})
		if err != nil {
			return err
		}
		if err := p.publish(ctx, EventApply, chunk); err != nil {
			return err
		}
	}
	return nil
}

// publish retries back-pressure and reports everything else.
//
// A full ring means the bus is busy, not broken. Reporting it as a failure
// would send the operation back to the outbox for redelivery over a condition
// that clears on its own in microseconds.
func (p *ApplyPublisher) publish(ctx context.Context, kind uint32, payload []byte) error {
	for {
		err := p.client.Publish(kind, payload)
		if err == nil {
			return nil
		}
		if !errors.Is(err, bus.ErrWouldBlock) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(p.pollWait):
		}
	}
}

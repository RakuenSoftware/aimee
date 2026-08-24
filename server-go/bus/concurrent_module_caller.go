package bus

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"
)

// ErrModuleCallNotDispatched marks a local send failure: no request reached
// the host, so a producer cannot have spent or executed work.
var ErrModuleCallNotDispatched = errors.New("module call was not dispatched")

// ConcurrentModuleCaller multiplexes concurrent stage calls over one attached
// requester. ModuleCaller intentionally owns a whole Client; this variant is
// for long-lived processes (the WFE and module-to-module requesters) that have
// one admitted principal but many simultaneous calls.
type ConcurrentModuleCaller struct {
	client     callerBus
	next       atomic.Uint64
	sendMu     sync.Mutex
	mu         sync.Mutex
	pending    map[uint64]concurrentPending
	assemblies map[uint64]concurrentAssembly
	isClosed   bool
	closed     chan struct{}
	// stopped is closed by the poll goroutine as it returns, so a caller can
	// wait for it to be gone before unmapping what it reads.
	stopped      chan struct{}
	closeOnce    sync.Once
	calls        sync.WaitGroup
	pollInterval time.Duration
}

type callerReply struct {
	body []byte
	err  error
}

type concurrentPending struct {
	eventKind uint32
	reply     chan callerReply
}

type concurrentAssembly struct{ body []byte }

func NewConcurrentModuleCaller(ctx context.Context, client *Client) (*ConcurrentModuleCaller, error) {
	if ctx == nil || client == nil {
		return nil, ErrModuleConfig
	}
	return newConcurrentModuleCaller(ctx, client), nil
}

func newConcurrentModuleCaller(ctx context.Context, client callerBus) *ConcurrentModuleCaller {
	c := &ConcurrentModuleCaller{client: client, pending: make(map[uint64]concurrentPending),
		assemblies: make(map[uint64]concurrentAssembly), closed: make(chan struct{}),
		stopped: make(chan struct{}), pollInterval: 200 * time.Microsecond}
	go c.poll(ctx)
	return c
}

func (c *ConcurrentModuleCaller) finish(err error) {
	c.closeOnce.Do(func() {
		c.mu.Lock()
		c.isClosed = true
		close(c.closed)
		for id, pending := range c.pending {
			delete(c.pending, id)
			pending.reply <- callerReply{err: err}
		}
		clear(c.assemblies)
		c.mu.Unlock()
	})
}

func (c *ConcurrentModuleCaller) poll(ctx context.Context) {
	defer close(c.stopped)
	idleDelay := c.pollInterval
	for {
		// Stop when asked, not only when the context ends or a read fails.
		// Without this the loop cannot be shut down at all, so a caller that
		// wants to detach has no way to wait for it to stop first.
		select {
		case <-c.closed:
			return
		default:
		}
		if err := ctx.Err(); err != nil {
			c.finish(errors.Join(ErrModuleCallCancelled, err))
			return
		}
		event, ok, err := c.client.Poll()
		if err != nil {
			c.finish(err)
			return
		}
		if !ok {
			time.Sleep(idleDelay)
			if idleDelay < 10*time.Millisecond {
				idleDelay *= 2
			}
			continue
		}
		idleDelay = c.pollInterval
		id := event.Frame.CorrelationID
		c.mu.Lock()
		pending, exists := c.pending[id]
		c.mu.Unlock()
		if !exists {
			continue
		}
		if event.Frame.EventKind == KindCapabilityAbsent {
			c.deliver(id, callerReply{err: ErrModuleCallCapabilityAbsent})
			continue
		}
		if event.Frame.EventKind == KindError {
			c.deliver(id, callerReply{err: ErrModuleCallRejected})
			continue
		}
		if event.Frame.EventKind != pending.eventKind {
			continue
		}
		message, err := DecodeModuleMessage(event.Payload)
		if err != nil || message.Operation != ModuleOpResult {
			continue
		}
		if message.Status != ModuleStatusOK {
			c.deliver(id, callerReply{err: &ModuleCallStatusError{Status: message.Status}})
			continue
		}
		start, end := ModuleMessageHeaderLen, ModuleMessageHeaderLen+int(message.BodyLen)
		if end > len(event.Payload) {
			c.deliver(id, callerReply{err: ErrModuleRuntime})
			continue
		}
		c.mu.Lock()
		a := c.assemblies[id]
		c.mu.Unlock()
		if uint64(len(a.body))+uint64(message.BodyLen) > uint64(ModuleMessageMaxBody) {
			c.deliver(id, callerReply{err: ErrModuleRuntime})
			continue
		}
		a.body = append(a.body, event.Payload[start:end]...)
		if event.Frame.HdrFlags&FMore != 0 {
			c.mu.Lock()
			if _, exists := c.pending[id]; exists {
				c.assemblies[id] = a
			}
			c.mu.Unlock()
			continue
		}
		c.deliver(id, callerReply{body: a.body})
	}
}

func (c *ConcurrentModuleCaller) deliver(id uint64, reply callerReply) {
	c.mu.Lock()
	pending, exists := c.pending[id]
	delete(c.pending, id)
	delete(c.assemblies, id)
	c.mu.Unlock()
	if exists {
		pending.reply <- reply
	}
}

func (c *ConcurrentModuleCaller) Call(ctx context.Context, eventKind, stageID uint32,
	traceID uint64, deadline time.Duration, request []byte) ([]byte, error) {
	if c == nil || c.client == nil || uint32(len(request)) > ModuleMessageMaxBody {
		return nil, ErrModuleConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	id := c.next.Add(1)
	ch := make(chan callerReply, 1)
	c.mu.Lock()
	if c.isClosed {
		c.mu.Unlock()
		return nil, ErrModuleRuntime
	}
	c.pending[id] = concurrentPending{eventKind: eventKind, reply: ch}
	c.calls.Add(1)
	c.mu.Unlock()
	defer c.calls.Done()

	var deadlineNS uint64
	if deadline > 0 {
		deadlineNS = monotonicNowNS() + uint64(deadline)
	}
	c.sendMu.Lock()
	err := (&ModuleCaller{client: c.client}).send(eventKind, stageID, traceID, id, deadlineNS, request)
	c.sendMu.Unlock()
	if err != nil {
		c.deliver(id, callerReply{err: errors.Join(ErrModuleCallNotDispatched, err)})
	}
	var timer <-chan time.Time
	var stop func() bool
	if deadline > 0 {
		t := time.NewTimer(deadline)
		timer, stop = t.C, t.Stop
		defer stop()
	}
	select {
	case reply := <-ch:
		return reply.body, reply.err
	case <-ctx.Done():
		c.abandon(eventKind, id)
		return nil, errors.Join(ErrModuleCallCancelled, ctx.Err())
	case <-timer:
		c.abandon(eventKind, id)
		return nil, ErrModuleCallDeadline
	case <-c.closed:
		c.abandon(eventKind, id)
		return nil, ErrModuleRuntime
	}
}

// CloseAndWait refuses new calls, releases pending calls, and blocks until both
// the admitted calls and the poll goroutine have finished. It must run before
// the underlying Client is detached and its shared-memory region is unmapped.
//
// TWO WAITS, because two things are still using that region and they finish
// independently. This branch and upstream each added a CloseAndWait waiting for
// one of them, and the merge duplicated the method -- which the Go compiler
// caught, and which is worth writing down because either version alone looks
// complete:
//
//	c.calls.Wait()  every call admitted before shutdown has returned, so no
//	                caller is still reading a reply out of the region
//	<-c.stopped     the poll goroutine has returned, so nothing is reading the
//	                ring itself
//
// Dropping the second is a read through an unmapped page on the ORDINARY exit
// path: poll is parked reading shared memory when Detach pulls it out from
// under it. NO IN-PROCESS TEST CAN FAIL ON IT -- a fake bus has no region to
// unmap -- and it was found on real hardware, as a fault arriving after the
// last assertion.
func (c *ConcurrentModuleCaller) CloseAndWait() {
	if c == nil {
		return
	}
	c.finish(ErrModuleRuntime)
	c.calls.Wait()
	<-c.stopped
}

func (c *ConcurrentModuleCaller) abandon(kind uint32, id uint64) {
	c.mu.Lock()
	delete(c.pending, id)
	delete(c.assemblies, id)
	c.mu.Unlock()
	c.sendMu.Lock()
	_ = c.client.Cancel(kind, id)
	c.sendMu.Unlock()
}

package bus

import (
	"context"
	"sync/atomic"
	"testing"
	"testing/synctest"
	"time"
)

type countedCallerBus struct {
	*fakeCallerBus
	polls atomic.Int64
}

func (b *countedCallerBus) Poll() (Event, bool, error) {
	b.polls.Add(1)
	return b.fakeCallerBus.Poll()
}

func TestConcurrentCallerRetainsIdleBackoffAndWakesForRequest(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		fake := &countedCallerBus{fakeCallerBus: echoingBus(128)}
		caller := newConcurrentModuleCaller(t.Context(), fake)
		defer caller.CloseAndWait()
		synctest.Wait()
		before := fake.polls.Load()
		time.Sleep(time.Second)
		synctest.Wait()
		if polls := fake.polls.Load() - before; polls > 120 {
			t.Fatalf("idle connection polled %d times in one second", polls)
		}
		start := time.Now()
		reply, err := caller.Call(t.Context(), 6657, 1, 1, time.Second, []byte("ready"))
		if err != nil || string(reply) != "ready" {
			t.Fatal(string(reply), err)
		}
		if elapsed := time.Since(start); elapsed != 0 {
			t.Fatalf("already-ready reply waited for idle polling timer: %v", elapsed)
		}
		fake.replyTo = func(f *fakeCallerBus, kind uint32, id uint64, body []byte) {
			go func() {
				time.Sleep(3 * time.Millisecond)
				f.queueResult(kind, id, ModuleStatusOK, body, 1)
			}()
		}
		start = time.Now()
		reply, err = caller.Call(t.Context(), 6657, 1, 2, time.Second, []byte("later"))
		if err != nil || string(reply) != "later" {
			t.Fatal(string(reply), err)
		}
		if elapsed := time.Since(start); elapsed > 5*time.Millisecond {
			t.Fatalf("pending reply inherited idle backoff: %v", elapsed)
		}
	})
}

type notifyingModuleBus struct {
	*fakeModuleBus
	published chan struct{}
}

func (b *notifyingModuleBus) ReplyFragment(kind uint32, id uint64, payload []byte, more bool) error {
	err := b.fakeModuleBus.ReplyFragment(kind, id, payload, more)
	b.published <- struct{}{}
	return err
}

func TestCompletedModuleHandlerWakesReplyPublisher(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		const kind uint32 = 5889
		fake := &notifyingModuleBus{fakeModuleBus: &fakeModuleBus{budget: 128}, published: make(chan struct{}, 1)}
		fake.input = []Event{moduleRequestEvent(t, kind, 1, ModuleMessage{Operation: ModuleOpInvoke, StageID: 1}, nil, false)}
		config := ModuleProcessConfig{Handler: func(ModuleInvocation, []byte) ([]byte, ModuleStatus) {
			time.Sleep(20 * time.Millisecond)
			return []byte("done"), ModuleStatusOK
		}}
		ctx, cancel := context.WithCancel(t.Context())
		defer cancel()
		stopped := make(chan error, 1)
		start := time.Now()
		go func() { stopped <- runModuleClient(ctx, config, map[uint32]uint32{kind: 1}, fake) }()
		<-fake.published
		if elapsed := time.Since(start); elapsed != 20*time.Millisecond {
			t.Fatalf("completed handler waited for ring polling timer: %v", elapsed)
		}
		cancel()
		if err := <-stopped; err != nil {
			t.Fatal(err)
		}
	})
}

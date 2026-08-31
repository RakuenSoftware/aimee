package routing

import (
	"encoding/binary"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func routingRequest(mode uint16, count uint32) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint16(request[4:6], wireVersion)
	binary.LittleEndian.PutUint16(request[6:8], mode)
	binary.LittleEndian.PutUint32(request[8:12], count)
	return request
}

func selectedIndex(t *testing.T, response []byte, status bus.ModuleStatus) uint32 {
	t.Helper()
	if status != bus.ModuleStatusOK || len(response) != responseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != responseMagic {
		t.Fatalf("response = %x, status = %d", response, status)
	}
	return binary.LittleEndian.Uint32(response[4:8])
}

func TestBalancedAndRandomizedSelection(t *testing.T) {
	selector := &Selector{}
	invocation := bus.ModuleInvocation{StageID: StageSelect, TraceID: 0x12345678}
	response, status := selector.Handle(invocation, routingRequest(selectBalanced, 3))
	if got := selectedIndex(t, response, status); got != 0 {
		t.Fatalf("first balanced index = %d", got)
	}
	response, status = selector.Handle(invocation, routingRequest(selectBalanced, 3))
	if got := selectedIndex(t, response, status); got != 1 {
		t.Fatalf("second balanced index = %d", got)
	}
	want := uint32(mix64(invocation.TraceID) % 5)
	response, status = selector.Handle(invocation, routingRequest(selectRandomized, 5))
	if got := selectedIndex(t, response, status); got != want {
		t.Fatalf("randomized index = %d, want %d", got, want)
	}
}

func TestBalancedSelectionIsConcurrent(t *testing.T) {
	selector := &Selector{}
	type result struct {
		response []byte
		status   bus.ModuleStatus
	}
	results := make(chan result, 64)
	var workers sync.WaitGroup
	for range 64 {
		workers.Add(1)
		go func() {
			defer workers.Done()
			response, status := selector.Handle(bus.ModuleInvocation{StageID: StageSelect},
				routingRequest(selectBalanced, 64))
			results <- result{response: response, status: status}
		}()
	}
	workers.Wait()
	close(results)
	unique := make(map[uint32]bool)
	for result := range results {
		unique[selectedIndex(t, result.response, result.status)] = true
	}
	if len(unique) != 64 {
		t.Fatalf("balanced cursor produced %d unique indexes", len(unique))
	}
}

func TestRoutingRejectsInvalidAndExpiredRequests(t *testing.T) {
	selector := &Selector{}
	if _, status := selector.Handle(bus.ModuleInvocation{StageID: StageSelect},
		routingRequest(99, 2)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("invalid mode status = %d", status)
	}
	if _, status := selector.Handle(bus.ModuleInvocation{StageID: StageSelect, DeadlineNS: 1},
		routingRequest(selectBalanced, 2)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := selector.Handle(bus.ModuleInvocation{StageID: StageSelect, DeadlineNS: 1},
		[]byte("malformed")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired malformed invocation status = %d", status)
	}
}

package workspace

import (
	"encoding/binary"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func ioRequest(op byte, id string, payload []byte) []byte {
	request := make([]byte, ioHeaderLen+len(id)+len(payload))
	binary.LittleEndian.PutUint32(request[0:4], ioRequestMagic)
	request[4] = wireVersion
	request[5] = op
	binary.LittleEndian.PutUint16(request[6:8], uint16(len(id)))
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(payload)))
	copy(request[ioHeaderLen:], id)
	copy(request[ioHeaderLen+len(id):], payload)
	return request
}

func io(op byte, id string, payload []byte) ([]byte, bus.ModuleStatus) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO},
		ioRequest(op, id, payload))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	length := int(binary.LittleEndian.Uint32(response[4:8]))
	return response[ioHeaderLen-4 : ioHeaderLen-4+length], status
}

// The whole point of the rendezvous: work the server needs done reaches the
// client holding the tree, and its result comes back to the caller that asked.
func TestRunnerIOCarriesAnOpToTheClientAndItsResultBack(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	var wg sync.WaitGroup
	wg.Add(1)
	var got []byte
	var status bus.ModuleStatus
	go func() {
		defer wg.Done()
		got, status = io(IOOpSubmit, "/srv/repo", []byte("rev-parse"))
	}()

	// The client side: claim the op, run it, post the result.
	var claimed []byte
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		next, pollStatus := io(IOOpPoll, "/srv/repo", nil)
		if pollStatus == bus.ModuleStatusOK && len(next) > 0 {
			claimed = next
			break
		}
	}
	if string(claimed) != "rev-parse" {
		t.Fatalf("client claimed %q, want the submitted op", claimed)
	}
	if _, respondStatus := io(IOOpRespond, "/srv/repo", []byte("/srv/repo")); respondStatus != bus.ModuleStatusOK {
		t.Fatalf("respond status = %d", respondStatus)
	}

	wg.Wait()
	if status != bus.ModuleStatusOK || string(got) != "/srv/repo" {
		t.Fatalf("submitter got %q status %d", got, status)
	}
}

// A tree nobody is serving must fail fast. Parking the caller on a rendezvous
// that will never be drained would wedge the turn instead of failing it.
func TestRunnerIORefusesATreeNobodyIsServing(t *testing.T) {
	reset()
	if _, status := io(IOOpSubmit, "/srv/unserved", []byte("op")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("submit to an unserved tree status = %d", status)
	}
	if _, status := io(IOOpPoll, "/srv/unserved", nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("poll on an unserved tree status = %d", status)
	}
}

// When the client goes away mid-op its submitter has to be released, not left
// waiting on a result that is never coming.
func TestRunnerIOReleasesASubmitterWhenTheClientLeaves(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	done := make(chan bus.ModuleStatus, 1)
	go func() {
		_, status := io(IOOpSubmit, "/srv/repo", []byte("op"))
		done <- status
	}()

	// Let the submitter park on the handoff, then pull the client out.
	time.Sleep(50 * time.Millisecond)
	call(t, RunnerOpUnregister, "/srv/repo")

	select {
	case status := <-done:
		if status == bus.ModuleStatusOK {
			t.Fatalf("submitter reported OK after its client left")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("submitter never returned after its client left")
	}
}

// A poll that elapses with nothing pending is the ordinary idle case; the client
// just polls again. It reports cancelled rather than OK-with-nothing because
// cancellation precedence belongs to the bus, and a module reinterpreting it
// would make an elapsed poll indistinguishable from a real answer of "no work".
func TestRunnerIOElapsedPollReportsCancelledSoTheClientRepolls(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	// DeadlineNS 1 is already past, so the wait ends immediately.
	_, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO, DeadlineNS: 1},
		ioRequest(IOOpPoll, "/srv/repo", nil))
	if status != bus.ModuleStatusCancelled {
		t.Fatalf("elapsed poll status = %d, want cancelled", status)
	}
}

// A response nobody is waiting for is a protocol error, not something to file
// against whatever op happens to come next.
func TestRunnerIORejectsAnUnclaimedResponse(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")
	if _, status := io(IOOpRespond, "/srv/repo", []byte("result")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unclaimed respond status = %d", status)
	}
}

func TestRunnerIORejectsInvalidEnvelope(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	short := ioRequest(IOOpPoll, "/srv/repo", nil)[:ioHeaderLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO}, short); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated-header status = %d", status)
	}

	// A length field that disagrees with the body it describes.
	lying := ioRequest(IOOpPoll, "/srv/repo", nil)
	binary.LittleEndian.PutUint32(lying[8:12], 64)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("payload-length-mismatch status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO},
		ioRequest(9, "/srv/repo", nil)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown-op status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO},
		ioRequest(IOOpPoll, strings.Repeat("a", runnerIDMax+1), nil)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("oversized-id status = %d", status)
	}
}

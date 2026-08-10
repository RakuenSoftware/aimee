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
	body, _, status := ioFull(op, id, payload)
	return body, status
}

// ioFull also reports whether more chunks of this result are still coming.
func ioFull(op byte, id string, payload []byte) ([]byte, bool, bus.ModuleStatus) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO},
		ioRequest(op, id, payload))
	if status != bus.ModuleStatusOK {
		return nil, false, status
	}
	more := binary.LittleEndian.Uint32(response[4:8])&ioMore != 0
	length := int(binary.LittleEndian.Uint32(response[8:12]))
	return response[ioRespHeaderLen : ioRespHeaderLen+length], more, status
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
//
// It must fail as CAPABILITY_ABSENT, NOT as INVALID_REQUEST. The distinction is
// load-bearing: the request is well formed, and a caller that cannot tell "you
// sent nonsense" from "nobody is serving this tree" treats the second as
// "nothing pending yet" and re-polls immediately. That is a permanent condition
// driving an unbounded retry loop — observed on a live appliance as 664,408
// identical log lines in 25 minutes, ~440/second, drowning every other line.
func TestRunnerIORefusesATreeNobodyIsServing(t *testing.T) {
	reset()
	if _, status := io(IOOpSubmit, "/srv/unserved", []byte("op")); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("submit to an unserved tree status = %d, want CapabilityAbsent", status)
	}
	if _, status := io(IOOpPoll, "/srv/unserved", nil); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("poll on an unserved tree status = %d, want CapabilityAbsent", status)
	}
	// A genuinely malformed request must still be INVALID_REQUEST, or the new
	// signal is useless — both cases would collapse again, the other way.
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunnerIO},
		[]byte("not a runner-io frame")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed frame status = %d, want InvalidRequest", status)
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

// A result that arrives in pieces. One response per request is the protocol, so
// the submitter is told there is more and pulls the rest; the alternative is
// buffering a whole LLM response before anyone sees a token of it.
func TestRunnerIOStreamsAResultInChunks(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	type outcome struct {
		chunks []string
		ok     bool
	}
	got := make(chan outcome, 1)
	go func() {
		var chunks []string
		body, more, status := ioFull(IOOpSubmit, "/srv/repo", []byte("run agent"))
		if status != bus.ModuleStatusOK {
			got <- outcome{nil, false}
			return
		}
		chunks = append(chunks, string(body))
		for more {
			body, more, status = ioFull(IOOpDrain, "/srv/repo", nil)
			if status != bus.ModuleStatusOK {
				got <- outcome{nil, false}
				return
			}
			chunks = append(chunks, string(body))
		}
		got <- outcome{chunks, true}
	}()

	// Client side: claim the op, then emit two partials and a final.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if next, status := io(IOOpPoll, "/srv/repo", nil); status == bus.ModuleStatusOK && len(next) > 0 {
			break
		}
	}
	for _, part := range []string{"tok1", "tok2"} {
		if _, status := io(IOOpRespondPartial, "/srv/repo", []byte(part)); status != bus.ModuleStatusOK {
			t.Fatalf("partial %q status = %d", part, status)
		}
	}
	if _, status := io(IOOpRespond, "/srv/repo", []byte("done")); status != bus.ModuleStatusOK {
		t.Fatalf("final status = %d", status)
	}

	select {
	case result := <-got:
		if !result.ok {
			t.Fatal("submitter failed mid-stream")
		}
		want := []string{"tok1", "tok2", "done"}
		if len(result.chunks) != len(want) {
			t.Fatalf("chunks = %v, want %v", result.chunks, want)
		}
		for i := range want {
			if result.chunks[i] != want[i] {
				t.Fatalf("chunks = %v, want %v", result.chunks, want)
			}
		}
	case <-time.After(5 * time.Second):
		t.Fatal("submitter never finished draining")
	}

	// The stream is over, so there is nothing left to pull.
	if _, _, status := ioFull(IOOpDrain, "/srv/repo", nil); status == bus.ModuleStatusOK {
		t.Fatal("drain after the final chunk succeeded")
	}
}

// A partial keeps the claim: the client is still producing, so the tree is not
// free for the next op yet.
func TestRunnerIOPartialKeepsTheClaimAndFinalReleasesIt(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	go func() { _, _, _ = ioFull(IOOpSubmit, "/srv/repo", []byte("op")) }()

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if next, status := io(IOOpPoll, "/srv/repo", nil); status == bus.ModuleStatusOK && len(next) > 0 {
			break
		}
	}
	if _, status := io(IOOpRespondPartial, "/srv/repo", []byte("a")); status != bus.ModuleStatusOK {
		t.Fatalf("first partial status = %d", status)
	}
	// Still claimed, so a second partial is accepted.
	if _, status := io(IOOpRespondPartial, "/srv/repo", []byte("b")); status != bus.ModuleStatusOK {
		t.Fatalf("second partial status = %d", status)
	}
	if _, status := io(IOOpRespond, "/srv/repo", []byte("z")); status != bus.ModuleStatusOK {
		t.Fatalf("final status = %d", status)
	}
	// Released: a further response belongs to no claim.
	if _, status := io(IOOpRespond, "/srv/repo", []byte("extra")); status == bus.ModuleStatusOK {
		t.Fatal("a response after the final one was accepted")
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

package workspace

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func runnerRequest(op byte, value string) []byte {
	request := make([]byte, runnerRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], runnerRequestMagic)
	request[4] = wireVersion
	request[5] = op
	binary.LittleEndian.PutUint16(request[6:8], uint16(len(value)))
	copy(request[8:], value)
	return request
}

// call runs one runner op and returns the id it resolved to ("" for none).
func call(t *testing.T, op byte, value string) string {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageRunner}, runnerRequest(op, value))
	if status != bus.ModuleStatusOK || len(response) != runnerRespLen ||
		binary.LittleEndian.Uint32(response[0:4]) != runnerResponseMagic {
		t.Fatalf("op %d on %q: response = %x, status = %d", op, value, response, status)
	}
	length := int(binary.LittleEndian.Uint32(response[4:8]))
	if length > runnerIDMax {
		t.Fatalf("op %d on %q: id length %d out of range", op, value, length)
	}
	return string(response[8 : 8+length])
}

func reset() {
	runnersMu.Lock()
	defer runnersMu.Unlock()
	runners = map[string]*rendezvous{}
}

// A client serving a tree is what makes that tree reachable, so resolving must
// answer from who is actually here — not from who was declared in advance.
func TestRunnerResolvesTheServingClientAndItsDescendants(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	if got := call(t, RunnerOpResolve, "/srv/repo"); got != "/srv/repo" {
		t.Errorf("the tree itself resolved to %q", got)
	}
	if got := call(t, RunnerOpResolve, "/srv/repo/src/main.c"); got != "/srv/repo" {
		t.Errorf("a path under it resolved to %q", got)
	}
}

// Without a component boundary, a client serving /srv/repo would be handed
// /srv/repo-backup and its writes would land in the wrong tree.
func TestRunnerWillNotClaimASiblingSharingAPrefix(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")

	for _, path := range []string{"/srv/repo-backup", "/srv/repository", "/srv/other", "/"} {
		if got := call(t, RunnerOpResolve, path); got != "" {
			t.Errorf("%q resolved to %q, want nobody", path, got)
		}
	}
}

// Nobody serving it is a real answer. Inventing one would strand the caller on a
// rendezvous no client is draining.
func TestRunnerReportsNobodyRatherThanGuessing(t *testing.T) {
	reset()
	if got := call(t, RunnerOpResolve, "/srv/never-served"); got != "" {
		t.Errorf("resolved to %q, want nobody", got)
	}
}

func TestRunnerPrefersTheClosestServingClient(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")
	call(t, RunnerOpRegister, "/srv/repo/vendor")

	if got := call(t, RunnerOpResolve, "/srv/repo/vendor/lib.c"); got != "/srv/repo/vendor" {
		t.Errorf("nested path resolved to %q, want the nested runner", got)
	}
	// The parent still serves what the child does not cover.
	if got := call(t, RunnerOpResolve, "/srv/repo/src/main.c"); got != "/srv/repo" {
		t.Errorf("sibling path resolved to %q, want the parent", got)
	}
}

// When the client goes away so does the answer, so a turn falls back instead of
// marshalling into a rendezvous nobody is draining.
func TestRunnerForgetsAClientThatStoppedServing(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")
	call(t, RunnerOpUnregister, "/srv/repo")

	if got := call(t, RunnerOpResolve, "/srv/repo/src/main.c"); got != "" {
		t.Errorf("resolved to %q after the client left", got)
	}
}

// Re-serving the same tree is normal (a reconnect), not an error.
func TestRunnerRegistrationIsIdempotentAndBounded(t *testing.T) {
	reset()
	call(t, RunnerOpRegister, "/srv/repo")
	call(t, RunnerOpRegister, "/srv/repo")
	if got := call(t, RunnerOpResolve, "/srv/repo"); got != "/srv/repo" {
		t.Errorf("resolved to %q after re-registering", got)
	}

	reset()
	for index := 0; index < runnerMax; index++ {
		call(t, RunnerOpRegister, "/srv/"+strings.Repeat("a", index+1))
	}
	_, status := Handle(bus.ModuleInvocation{StageID: StageRunner},
		runnerRequest(RunnerOpRegister, "/srv/one-too-many"))
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("registering past the ceiling status = %d", status)
	}
}

func TestRunnerRejectsInvalidEnvelopeAndHonorsCancellationPrecedence(t *testing.T) {
	reset()
	bad := runnerRequest(RunnerOpResolve, "/srv/repo")
	binary.LittleEndian.PutUint32(bad[0:4], requestMagic) // the other stage's magic
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunner}, bad); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-magic status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunner},
		runnerRequest(9, "/srv/repo")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown-op status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunner},
		runnerRequest(RunnerOpResolve, "")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("empty-path status = %d", status)
	}

	// An id has to fit the rendezvous key the transport looks it up by.
	long := runnerRequest(RunnerOpRegister, strings.Repeat("a", runnerIDMax+1))
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunner}, long); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("oversized-id status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRunner, DeadlineNS: 1},
		runnerRequest(RunnerOpResolve, "/srv/repo")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired status = %d", status)
	}
}

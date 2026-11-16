// Package workspace implements the workspace-access process wire contract.
package workspace

import (
	"encoding/binary"
	"sync"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const (
	EventKind uint32 = 7169
	// One kind per stage: the bus maps an event kind to exactly one stage, so
	// stages cannot share a kind and be told apart by their magic.
	EventRunner   uint32 = 7170
	EventRunnerIO uint32 = 7171
	StageAccess   uint32 = 1
	StageRunner   uint32 = 2
	requestMagic  uint32 = 0x46455257
	responseMagic uint32 = 0x4b4f5757
	wireVersion   byte   = 1
	refMax               = 129
	requestLen           = 140
	responseLen          = 8

	runnerRequestMagic  uint32 = 0x4e555257 /* "WRUN" */
	runnerResponseMagic uint32 = 0x56535257 /* "WRSV" */
	// Ids are bounded to match the rendezvous key the transport carries.
	runnerIDMax      = 128
	runnerPathMax    = 1024
	runnerRequestLen = 8 + runnerPathMax
	runnerRespLen    = 8 + runnerIDMax
	// The same ceiling the transport keeps on live runners.
	runnerMax = 64

	RunnerOpRegister   byte = 1
	RunnerOpUnregister byte = 2
	RunnerOpResolve    byte = 3
)

// The set of trees a client is serving right now. This is policy, so it lives
// here rather than beside the transport: whether a turn may act on a path is a
// decision, and the only thing the bus side keeps is the socket rendezvous it
// looks up by the id this returns.
var (
	runnersMu sync.Mutex
	runners   = map[string]*rendezvous{}
)

// covers reports whether a runner serving id also serves path — that is, id is
// path itself or a parent directory of it. The comparison is on whole path
// components, so a runner serving /srv/repo is never handed /srv/repo-backup.
func covers(id, path string) bool {
	if len(id) == 0 || len(id) > len(path) || path[:len(id)] != id {
		return false
	}
	// A trailing '/' on the id has already consumed the boundary.
	return len(id) == len(path) || path[len(id)] == '/' || id[len(id)-1] == '/'
}

// serverFor returns the id of the runner serving path — the closest parent when
// several match, so a nested runner beats the one above it — or "" when nobody
// is serving it. An empty answer is a real answer: it means no client is here to
// do the work, and the caller must not invent one.
func serverFor(path string) string {
	runnersMu.Lock()
	defer runnersMu.Unlock()
	best := ""
	for id := range runners {
		if covers(id, path) && len(id) > len(best) {
			best = id
		}
	}
	return best
}

func runnerRegister(id string) bool {
	runnersMu.Lock()
	defer runnersMu.Unlock()
	if _, ok := runners[id]; ok {
		return true // idempotent: re-serving the same tree is not an error
	}
	if len(runners) >= runnerMax {
		return false
	}
	runners[id] = newRendezvous()
	return true
}

func runnerUnregister(id string) {
	runnersMu.Lock()
	point := runners[id]
	delete(runners, id)
	runnersMu.Unlock()
	// Outside the lock: closing wakes anyone blocked on the handoff and fails
	// their op, rather than leaving them parked on a client that has gone.
	if point != nil {
		point.close()
	}
}

// A runner channel is one client's private rendezvous: "runner:" and 32
// lowercase hex digits the client chose. The aimee CLI opens one for each
// command it runs inside a detached tree and sends its name with that command,
// so the ops the command causes are run by that client and no other. A tree's
// own id stays shared by `aimee workspace serve`; a CLI that polled it could
// otherwise be handed another session's op and run it with its own
// environment and credentials.
const (
	runnerChannelPrefix = "runner:"
	runnerChannelLen    = len(runnerChannelPrefix) + 32
	// Channels are per command, so many are open at once across sessions.
	runnerChannelMax = 1024
	// A channel nobody polled for this long belongs to a client that is gone.
	runnerChannelIdle = 2 * time.Minute
)

var channels = map[string]*rendezvous{}

func isRunnerChannel(id string) bool {
	if len(id) != runnerChannelLen || id[:len(runnerChannelPrefix)] != runnerChannelPrefix {
		return false
	}
	for _, c := range []byte(id[len(runnerChannelPrefix):]) {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// channelFor returns a channel's rendezvous, opening it on first use: the
// client's own first poll opens it, so the command it then sends finds it
// served. Channels idle past runnerChannelIdle are closed on the way.
func channelFor(id string) *rendezvous {
	runnersMu.Lock()
	defer runnersMu.Unlock()
	if point := channels[id]; point != nil {
		return point
	}
	for other, point := range channels {
		if point.idleFor() > runnerChannelIdle {
			delete(channels, other)
			point.close()
		}
	}
	if len(channels) >= runnerChannelMax {
		return nil
	}
	point := newRendezvous()
	point.markServing()
	channels[id] = point
	return point
}

func channelClose(id string) {
	runnersMu.Lock()
	point := channels[id]
	delete(channels, id)
	runnersMu.Unlock()
	if point != nil {
		point.close()
	}
}

func runnerFor(id string) *rendezvous {
	if isRunnerChannel(id) {
		return channelFor(id)
	}
	runnersMu.Lock()
	defer runnersMu.Unlock()
	return runners[id]
}

func handleRunner(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	if len(request) != runnerRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != runnerRequestMagic ||
		request[4] != wireVersion {
		return nil, ring.ModuleStatusInvalidRequest
	}
	op := request[5]
	length := int(binary.LittleEndian.Uint16(request[6:8]))
	if length == 0 || length > runnerPathMax {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if op != RunnerOpRegister && op != RunnerOpUnregister && op != RunnerOpResolve {
		return nil, ring.ModuleStatusInvalidRequest
	}
	// A registered id is the rendezvous key the transport looks up, so it has to
	// fit that key; a path being asked about does not.
	if (op == RunnerOpRegister || op == RunnerOpUnregister) && length > runnerIDMax {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}

	value := string(request[8 : 8+length])
	response := make([]byte, runnerRespLen)
	binary.LittleEndian.PutUint32(response[0:4], runnerResponseMagic)

	switch op {
	case RunnerOpRegister:
		if isRunnerChannel(value) {
			if channelFor(value) == nil {
				return nil, ring.ModuleStatusInvalidRequest
			}
		} else if !runnerRegister(value) {
			return nil, ring.ModuleStatusInvalidRequest
		}
	case RunnerOpUnregister:
		if isRunnerChannel(value) {
			channelClose(value)
		} else {
			runnerUnregister(value)
		}
	case RunnerOpResolve:
		if id := serverFor(value); id != "" {
			binary.LittleEndian.PutUint32(response[4:8], uint32(len(id)))
			copy(response[8:], id)
		}
	}
	return response, ring.ModuleStatusOK
}

func asciiAlphanumeric(value byte) bool {
	return value >= 'A' && value <= 'Z' || value >= 'a' && value <= 'z' ||
		value >= '0' && value <= '9'
}

func nameValid(name []byte) bool {
	if len(name) == 0 || len(name) > 64 || string(name) == "." || string(name) == ".." ||
		!asciiAlphanumeric(name[0]) {
		return false
	}
	for _, value := range name {
		if !asciiAlphanumeric(value) && value != '.' && value != '_' && value != '-' {
			return false
		}
	}
	return true
}

func refValid(ref []byte) bool {
	if len(ref) == 0 || len(ref) > refMax {
		return false
	}
	slash := -1
	for index, value := range ref {
		if value == 0 {
			return false
		}
		if value == '/' {
			if slash >= 0 {
				return false
			}
			slash = index
		}
	}
	if slash < 0 {
		return nameValid(ref)
	}
	return nameValid(ref[:slash]) && nameValid(ref[slash+1:])
}

// Handle validates a bounded project or owner/project reference, or answers
// which client is serving a tree.
func Handle(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	if invocation.StageID == StageRunner {
		return handleRunner(invocation, request)
	}
	if invocation.StageID == StageRunnerIO {
		return handleRunnerIO(invocation, request)
	}
	if invocation.StageID != StageAccess || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 {
		return nil, ring.ModuleStatusInvalidRequest
	}
	length := int(binary.LittleEndian.Uint16(request[6:8]))
	if length == 0 || length > refMax {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	if refValid(request[8 : 8+length]) {
		binary.LittleEndian.PutUint32(response[4:8], 1)
	}
	return response, ring.ModuleStatusOK
}

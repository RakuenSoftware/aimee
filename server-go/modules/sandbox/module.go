package sandbox

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// NewHandler serves the sandbox module's learned-toolchain and proxy-policy stages.
//
// All carry JSON because commands, git roots, HTTP heads, allowlists, and
// addresses are variable-length.
func NewHandler(store *Store) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		switch invocation.StageID {
		case StageObserve:
			if store == nil {
				return nil, bus.ModuleStatusInternal
			}
			return handleObserve(store, invocation, request)
		case StageLoad:
			if store == nil {
				return nil, bus.ModuleStatusInternal
			}
			return handleLoad(store, invocation, request)
		case StageProxyRequest:
			return handleProxyRequest(invocation, request)
		case StageProxyAddress:
			return handleProxyAddress(invocation, request)
		default:
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
}

func handleObserve(store *Store, invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded ObserveRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if decoded.GitRoot == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	parsed := ParseApt(decoded.Command, LearnMax)
	response := ObserveResponse{Parsed: len(parsed), Packages: parsed}
	if len(parsed) > 0 {
		// A store failure is reported, not swallowed: the caller treats learning
		// as best-effort, but "it did not persist" must be distinguishable from
		// "there was nothing to learn".
		added, err := store.Record(decoded.GitRoot, parsed)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		response.Recorded = added
	}
	return encode(response)
}

func handleLoad(store *Store, invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded LoadRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if decoded.GitRoot == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	return encode(LoadResponse{Packages: store.Load(decoded.GitRoot, LearnMax)})
}

func encode(value any) ([]byte, bus.ModuleStatus) {
	encoded, err := json.Marshal(value)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	if uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}

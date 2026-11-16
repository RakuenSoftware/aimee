package sandbox

import (
	"encoding/json"
	"log"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// NewHandler serves the sandbox module's learned-toolchain and proxy-policy stages.
//
// All carry JSON because commands, git roots, HTTP heads, allowlists, and
// addresses are variable-length.
func NewHandler(store *Store) ring.ModuleHandler {
	return func(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
		switch invocation.StageID {
		case StageObserve:
			if store == nil {
				return nil, ring.ModuleStatusInternal
			}
			return handleObserve(store, invocation, request)
		case StageLoad:
			if store == nil {
				return nil, ring.ModuleStatusInternal
			}
			return handleLoad(store, invocation, request)
		case StageProxyRequest:
			return handleProxyRequest(invocation, request)
		case StageProxyAddress:
			return handleProxyAddress(invocation, request)
		default:
			return nil, ring.ModuleStatusInvalidRequest
		}
	}
}

func handleObserve(store *Store, invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	var decoded ObserveRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if decoded.GitRoot == "" {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	parsed := ParseApt(decoded.Command, LearnMax)
	response := ObserveResponse{Parsed: len(parsed), Packages: parsed}
	if len(parsed) > 0 {
		// A store failure is reported, not swallowed: the caller treats learning
		// as best-effort, but "it did not persist" must be distinguishable from
		// "there was nothing to learn".
		added, err := store.Record(decoded.GitRoot, parsed)
		if err != nil {
			// The comment above asks for "it did not persist" to be
			// distinguishable from "there was nothing to learn"; a bare Internal
			// with the reason dropped cannot carry that distinction to anyone
			// reading a log.
			log.Printf("sandbox: recording learned packages for %q failed: %v",
				decoded.GitRoot, err)
			return nil, ring.ModuleStatusInternal
		}
		response.Recorded = added
	}
	return encode(response)
}

func handleLoad(store *Store, invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	var decoded LoadRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if decoded.GitRoot == "" {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	return encode(LoadResponse{Packages: store.Load(decoded.GitRoot, LearnMax)})
}

func encode(value any) ([]byte, ring.ModuleStatus) {
	encoded, err := json.Marshal(value)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	if uint32(len(encoded)) > ring.ModuleMessageMaxBody {
		return nil, ring.ModuleStatusInternal
	}
	return encoded, ring.ModuleStatusOK
}

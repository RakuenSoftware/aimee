package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// How deep a delegation chain may go, and when an inherited depth is stale.
//
// Depth is carried between processes in an environment variable, so a child
// spawned by a delegate inherits it and the limit holds across process
// boundaries. Reading and writing that variable is the caller's business; which
// answer it implies is this module's.

const (
	StageChain uint32 = 3
	EventChain uint32 = 6659

	chainRequestMagic  uint32 = 0x4e484344 /* "DCHN" */
	chainResponseMagic uint32 = 0x52484344 /* "DCHR" */
	chainRequestLen           = 20
	chainResponseLen          = 12

	ChainOpShouldClear byte = 1
	ChainOpCheckDepth  byte = 2
)

// ChainEnvShouldClear reports whether an inherited delegation depth is stale and
// must be dropped rather than believed.
//
// Two ways it goes stale. A depth with no parent marker is a leftover from a
// process that is gone: nothing vouches for it. A parent marker whose process is
// known not to be running is worse, because it names a specific ancestor that
// has since exited. Either way the chain being described no longer exists, and
// inheriting its depth would either forbid a legitimate delegation or let a real
// one past the limit.
//
// parentActiveKnown separates "the parent is gone" from "we could not tell".
// Only a definite answer clears; an unknown one leaves the depth alone, because
// discarding it on a failed check would quietly raise the ceiling.
func ChainEnvShouldClear(hasDepth, hasParent, parentActiveKnown, parentActive bool) bool {
	if hasDepth && !hasParent {
		return true
	}
	return hasParent && parentActiveKnown && !parentActive
}

// ChainDepthAllows reports the depth this delegation would run at and whether
// that is within the limit. The answer is the child's depth, not the parent's:
// a delegation at the limit is refused before it starts, not after.
func ChainDepthAllows(parentDepth, maxDepth int32) (current int32, allowed bool) {
	current = parentDepth + 1
	return current, current <= maxDepth
}

func boolByte(v bool) byte {
	if v {
		return 1
	}
	return 0
}

func handleChain(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != chainRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != chainRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	op := request[5]
	if op != ChainOpShouldClear && op != ChainOpCheckDepth {
		return nil, bus.ModuleStatusInvalidRequest
	}
	for _, flag := range request[6:10] {
		if flag > 1 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	response := make([]byte, chainResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], chainResponseMagic)
	switch op {
	case ChainOpShouldClear:
		clear := ChainEnvShouldClear(request[6] == 1, request[7] == 1, request[8] == 1,
			request[9] == 1)
		response[4] = boolByte(clear)
	default: // ChainOpCheckDepth
		parent := int32(binary.LittleEndian.Uint32(request[12:16]))
		max := int32(binary.LittleEndian.Uint32(request[16:20]))
		current, allowed := ChainDepthAllows(parent, max)
		response[4] = boolByte(allowed)
		binary.LittleEndian.PutUint32(response[8:12], uint32(current))
	}
	return response, bus.ModuleStatusOK
}

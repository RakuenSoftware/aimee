package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Which agents in a fleet may serve a delegate packet.
//
// The whole fleet goes at once because the modality relaxation is a decision
// ABOUT the fleet: it fires only when requiring modality would leave nobody and
// dropping it would leave somebody. A per-agent call could not answer it, and a
// caller that answered it itself would hold the interesting half of the rule.

const (
	StageRouteFilter uint32 = 17
	EventRouteFilter uint32 = 6673

	routeFilterRequestMagic  uint32 = 0x51525444 /* "DTRQ" */
	routeFilterResponseMagic uint32 = 0x53525444 /* "DTRS" */
	routeFilterReqHeaderLen         = 24
	routeFilterAgentLen             = 32

	routeFilterMaxAgents = 4096

	routeFilterFlagEnabled    uint32 = 1
	routeFilterFlagHasRole    uint32 = 2
	routeFilterFlagHaveCap    uint32 = 4
	routeFilterFlagDeprecated uint32 = 8
	routeFilterFlagTools      uint32 = 16
)

// handleRouteFilter returns a keep decision per agent, in the order sent.
func handleRouteFilter(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < routeFilterReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != routeFilterRequestMagic ||
		request[4] != wireVersion || request[5] > 1 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	dropDeprecated := request[5] == 1
	count := int(binary.LittleEndian.Uint32(request[8:12]))
	requiredCaps := binary.LittleEndian.Uint32(request[12:16])
	minContext := int(int32(binary.LittleEndian.Uint32(request[16:20])))

	if count > routeFilterMaxAgents ||
		len(request) != routeFilterReqHeaderLen+count*routeFilterAgentLen {
		return nil, bus.ModuleStatusInvalidRequest
	}

	agents := make([]RouteAgent, count)
	for i := 0; i < count; i++ {
		at := routeFilterReqHeaderLen + i*routeFilterAgentLen
		flags := binary.LittleEndian.Uint32(request[at : at+4])
		agents[i] = RouteAgent{
			Enabled:         flags&routeFilterFlagEnabled != 0,
			HasRole:         flags&routeFilterFlagHasRole != 0,
			HaveCap:         flags&routeFilterFlagHaveCap != 0,
			Deprecated:      flags&routeFilterFlagDeprecated != 0,
			ToolsEnabled:    flags&routeFilterFlagTools != 0,
			CapFlags:        binary.LittleEndian.Uint32(request[at+4 : at+8]),
			OverrideContext: int(int32(binary.LittleEndian.Uint32(request[at+8 : at+12]))),
			CatalogContext:  int(int32(binary.LittleEndian.Uint32(request[at+12 : at+16]))),
			CLIContext:      int(int32(binary.LittleEndian.Uint32(request[at+16 : at+20]))),
		}
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	result := RouteFilter(agents, requiredCaps, minContext, dropDeprecated)

	response := make([]byte, 16+count*4)
	binary.LittleEndian.PutUint32(response[0:4], routeFilterResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(result.Kept))
	putBool(response[8:12], result.Relaxed)
	binary.LittleEndian.PutUint32(response[12:16], result.EffectiveCaps)
	for i, keep := range result.Keep {
		putBool(response[16+i*4:20+i*4], keep)
	}
	return response, bus.ModuleStatusOK
}

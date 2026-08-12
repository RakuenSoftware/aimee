package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func routeFilterRequest(agents []RouteAgent, caps uint32, minContext int, drop bool) []byte {
	out := make([]byte, routeFilterReqHeaderLen+len(agents)*routeFilterAgentLen)
	binary.LittleEndian.PutUint32(out[0:4], routeFilterRequestMagic)
	out[4] = wireVersion
	if drop {
		out[5] = 1
	}
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(agents)))
	binary.LittleEndian.PutUint32(out[12:16], caps)
	binary.LittleEndian.PutUint32(out[16:20], uint32(int32(minContext)))

	for i, a := range agents {
		at := routeFilterReqHeaderLen + i*routeFilterAgentLen
		var flags uint32
		if a.Enabled {
			flags |= routeFilterFlagEnabled
		}
		if a.HasRole {
			flags |= routeFilterFlagHasRole
		}
		if a.HaveCap {
			flags |= routeFilterFlagHaveCap
		}
		if a.Deprecated {
			flags |= routeFilterFlagDeprecated
		}
		if a.ToolsEnabled {
			flags |= routeFilterFlagTools
		}
		binary.LittleEndian.PutUint32(out[at:at+4], flags)
		binary.LittleEndian.PutUint32(out[at+4:at+8], a.CapFlags)
		binary.LittleEndian.PutUint32(out[at+8:at+12], uint32(int32(a.OverrideContext)))
		binary.LittleEndian.PutUint32(out[at+12:at+16], uint32(int32(a.CatalogContext)))
		binary.LittleEndian.PutUint32(out[at+16:at+20], uint32(int32(a.CLIContext)))
	}
	return out
}

func callRouteFilter(t *testing.T, agents []RouteAgent, caps uint32, minContext int,
	drop bool) RouteFilterResult {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageRouteFilter},
		routeFilterRequest(agents, caps, minContext, drop))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(response) != 16+len(agents)*4 ||
		binary.LittleEndian.Uint32(response[0:4]) != routeFilterResponseMagic {
		t.Fatal("bad response")
	}
	out := RouteFilterResult{
		Kept:          int(binary.LittleEndian.Uint32(response[4:8])),
		Relaxed:       binary.LittleEndian.Uint32(response[8:12]) == 1,
		EffectiveCaps: binary.LittleEndian.Uint32(response[12:16]),
		Keep:          make([]bool, len(agents)),
	}
	for i := range agents {
		out.Keep[i] = binary.LittleEndian.Uint32(response[16+i*4:20+i*4]) == 1
	}
	return out
}

func TestRouteFilterStageCarriesTheDecision(t *testing.T) {
	vision := capableAgent()
	textOnly := capableAgent()
	textOnly.CapFlags = ModelCapTools

	got := callRouteFilter(t, []RouteAgent{textOnly, vision}, ModelCapTools|ModelCapVision, 0, false)
	if got.Relaxed || got.Kept != 1 || got.Keep[0] || !got.Keep[1] {
		t.Errorf("%+v, want only the vision agent kept and no relaxation", got)
	}
}

// The fleet-wide part has to survive the wire, including the flag saying it
// happened and the caps that were actually enforced.
func TestRouteFilterStageReportsRelaxation(t *testing.T) {
	textOnly := capableAgent()
	textOnly.CapFlags = ModelCapTools

	got := callRouteFilter(t, []RouteAgent{textOnly}, ModelCapTools|ModelCapVision, 0, false)
	if !got.Relaxed {
		t.Error("relaxation did not survive the wire")
	}
	if got.EffectiveCaps != ModelCapTools {
		t.Errorf("effective caps = %d, want the hard set", got.EffectiveCaps)
	}
	if got.Kept != 1 || !got.Keep[0] {
		t.Errorf("%+v, want the text agent kept", got)
	}
}

// Every context source has to arrive intact, or an agent is judged on a window
// it does not have.
func TestRouteFilterStageCarriesEveryContextSource(t *testing.T) {
	cliOnly := RouteAgent{Enabled: true, HasRole: true, ToolsEnabled: true, CLIContext: 128000}
	if got := callRouteFilter(t, []RouteAgent{cliOnly}, 0, 100000, false); got.Kept != 1 {
		t.Errorf("the CLI-declared window did not reach the rule: %+v", got)
	}
	if got := callRouteFilter(t, []RouteAgent{cliOnly}, 0, 200000, false); got.Kept != 0 {
		t.Errorf("an agent under the minimum was kept: %+v", got)
	}

	override := RouteAgent{Enabled: true, HasRole: true, ToolsEnabled: true, HaveCap: true,
		CatalogContext: 8000, OverrideContext: 400000}
	if got := callRouteFilter(t, []RouteAgent{override}, 0, 200000, false); got.Kept != 1 {
		t.Errorf("the override window did not win over the catalog: %+v", got)
	}
}

func TestRouteFilterStageCarriesDropDeprecated(t *testing.T) {
	old := capableAgent()
	old.Deprecated = true
	if got := callRouteFilter(t, []RouteAgent{old}, 0, 0, true); got.Kept != 0 {
		t.Errorf("a deprecated model survived: %+v", got)
	}
	if got := callRouteFilter(t, []RouteAgent{old}, 0, 0, false); got.Kept != 1 {
		t.Errorf("a deprecated model was dropped unasked: %+v", got)
	}
}

func TestRouteFilterStageRejectsMalformedRequests(t *testing.T) {
	good := routeFilterRequest([]RouteAgent{capableAgent()}, ModelCapTools, 0, false)

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:16],
		"trailing byte": append(append([]byte{}, good...), 0),
		"truncated":     good[:len(good)-1],
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	// A count that does not match the body is the dangerous one: it would read
	// agents that were never sent.
	lying := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(lying[8:12], 5)
	cases["count disagrees with the body"] = lying

	tooMany := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(tooMany[8:12], routeFilterMaxAgents+1)
	cases["agent count over the bound"] = tooMany

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRouteFilter}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestRouteFilterStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageRouteFilter, DeadlineNS: 1}
	request := routeFilterRequest([]RouteAgent{capableAgent()}, ModelCapTools, 0, false)
	if _, status := Handle(invocation, request); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}

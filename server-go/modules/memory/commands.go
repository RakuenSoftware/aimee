package memory

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// The memory module declares the commands it owns.
//
// A command is registered ONCE, by the module that owns it, and every surface --
// CLI, the v1 RPC routes, MCP, ACP -- routes from that declaration. If a command
// is not declared here it is unroutable everywhere, which is the property that
// did not hold before: capability surface was written out five separate times by
// hand (231 RPC handlers, 162 CLI routes, 83 MCP dispatch entries, a 19-name
// shown-tools list, and ACP), nothing derived one from another, and they
// disagreed. `memory_get` was reachable from the CLI and registered for MCP
// dispatch yet absent from the surface an agent is shown; the standing guidance
// named three tools no external client could call.
//
// The declaration crosses the EVENT BUS like every other module answer. There is
// no in-process registration call: a module never reaches into core and never
// reaches another module. An earlier attempt at this linked a C file into core to
// register these directly, which is the arrangement this replaces.
//
// WIRE FORMAT, stated explicitly because the delegate mount-table bug is what
// happens when two sides assume different encodings and neither errors:
//
//	request  magic u32 | version u32                      (8 bytes, no payload)
//	response magic u32 | version u32 | count u32
//	         then per command:
//	           surfaces u32 | visibility u32
//	           group_len u16 | verb_len u16 | summary_len u16 | pad u16
//	           group bytes | verb bytes | summary bytes
//
// All integers little-endian, matching every other stage in this module. Strings
// are NOT NUL-terminated; their length prefixes are authoritative.
const (
	EventDeclareCommands uint32 = 5894
	StageDeclareCommands uint32 = 6

	commandsRequestMagic  uint32 = 0x444d4344 // "DCMD"
	commandsResponseMagic uint32 = 0x524d4344 // "DCMR"
	commandsRequestLen           = 8
)

// Surface bits. These MUST match aimee_surface_t in src/headers/command_registry.h;
// a command declared for a surface the core does not recognise is silently
// unroutable there, which is exactly the failure mode this whole table exists to
// remove.
const (
	SurfaceCLI uint32 = 1 << 0
	SurfaceRPC uint32 = 1 << 1
	SurfaceMCP uint32 = 1 << 2
	SurfaceACP uint32 = 1 << 3
)

// MCP visibility. Matches aimee_mcp_visibility_t.
const (
	MCPProminent    uint32 = 0 // listed in tools/list
	MCPDiscoverable uint32 = 1 // reachable via find_tools/describe_tool/call_tool
)

// Command is one declared verb. The CLI spelling (`aimee memory get`), the RPC
// spelling ("memory.get") and the MCP spelling (tool `memory`, command=get) are
// all derived from Group+Verb -- not maintained separately, which is how
// memory.recall became memory_recall while memory.search became search_memory,
// verb first, leaving no mechanical mapping between the surfaces.
type Command struct {
	Group      string
	Verb       string
	Summary    string
	Surfaces   uint32
	Visibility uint32
}

// declaredCommands is what this module owns.
//
// The surface masks are decisions, recorded here rather than implied by absence
// from some other list. store/list/delete are CLI+RPC only: writing and deleting
// memories is not something an agent should reach for mid-turn -- aimee decides
// what is worth keeping through the curator, and an agent that thinks a memory is
// wrong should supersede it, which keeps the history the curator reasons over.
var declaredCommands = []Command{
	{Group: "memory", Verb: "get", Summary: "Read one memory by id.",
		Surfaces: SurfaceCLI | SurfaceRPC | SurfaceMCP | SurfaceACP, Visibility: MCPDiscoverable},
	{Group: "memory", Verb: "search", Summary: "Search memories.",
		Surfaces: SurfaceCLI | SurfaceRPC | SurfaceMCP | SurfaceACP, Visibility: MCPProminent},
	{Group: "memory", Verb: "recall", Summary: "Recall memories for the current turn.",
		Surfaces: SurfaceCLI | SurfaceRPC | SurfaceMCP | SurfaceACP, Visibility: MCPProminent},
	{Group: "memory", Verb: "briefing", Summary: "Assemble the memory briefing.",
		Surfaces: SurfaceCLI | SurfaceRPC | SurfaceMCP | SurfaceACP, Visibility: MCPDiscoverable},
	{Group: "memory", Verb: "store", Summary: "Store a memory.",
		Surfaces: SurfaceCLI | SurfaceRPC, Visibility: MCPDiscoverable},
	{Group: "memory", Verb: "list", Summary: "List memories by tier/kind.",
		Surfaces: SurfaceCLI | SurfaceRPC, Visibility: MCPDiscoverable},
	{Group: "memory", Verb: "delete", Summary: "Delete a memory by id.",
		Surfaces: SurfaceCLI | SurfaceRPC, Visibility: MCPDiscoverable},
}

// DeclaredCommands exposes the set for tests and for callers assembling a view.
func DeclaredCommands() []Command { return declaredCommands }

func appendString(buf []byte, s string) []byte { return append(buf, s...) }

// handleDeclareCommands answers the declaration request.
func handleDeclareCommands(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < commandsRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != commandsRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		// Refuse rather than answer a request we did not understand. A malformed
		// declaration that is accepted anyway would register a partial command
		// surface, and a MISSING command is invisible -- it reads as "aimee cannot
		// do that" rather than as an error.
		return nil, bus.ModuleStatusInvalidRequest
	}

	out := make([]byte, 0, 12+len(declaredCommands)*64)
	var hdr [12]byte
	binary.LittleEndian.PutUint32(hdr[0:4], commandsResponseMagic)
	binary.LittleEndian.PutUint32(hdr[4:8], wireVersion)
	binary.LittleEndian.PutUint32(hdr[8:12], uint32(len(declaredCommands)))
	out = append(out, hdr[:]...)

	for _, c := range declaredCommands {
		var rec [16]byte
		binary.LittleEndian.PutUint32(rec[0:4], c.Surfaces)
		binary.LittleEndian.PutUint32(rec[4:8], c.Visibility)
		binary.LittleEndian.PutUint16(rec[8:10], uint16(len(c.Group)))
		binary.LittleEndian.PutUint16(rec[10:12], uint16(len(c.Verb)))
		binary.LittleEndian.PutUint16(rec[12:14], uint16(len(c.Summary)))
		binary.LittleEndian.PutUint16(rec[14:16], 0) // pad, keeps records 4-aligned
		out = append(out, rec[:]...)
		out = appendString(out, c.Group)
		out = appendString(out, c.Verb)
		out = appendString(out, c.Summary)
	}
	return out, bus.ModuleStatusOK
}

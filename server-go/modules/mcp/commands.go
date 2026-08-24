package mcp

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"strings"
)

// Command declaration for a plugin module.
//
// This is the SAME wire contract the memory module answers (see
// server-go/modules/memory/commands.go) and the same registry it lands in
// (src/headers/command_registry.h). Reusing it is the point: the registry's
// whole purpose is that a command is registered once and every surface -- CLI,
// v1 RPC, MCP, ACP -- routes from that one declaration. A second format for
// plugin commands would rebuild exactly the divergence the registry removed.
//
//	request  magic u32 | version u32                      (8 bytes, no payload)
//	response magic u32 | version u32 | count u32
//	         then per command:
//	           surfaces u32 | visibility u32
//	           group_len u16 | verb_len u16 | summary_len u16 | pad u16
//	           group bytes | verb bytes | summary bytes
//
// All integers little-endian. Strings are NOT NUL-terminated; the length
// prefixes are authoritative.
//
// WHAT DIFFERS from memory's: memory declares a static table known at compile
// time. A plugin module cannot -- its commands are whatever the plugin it hosts
// advertises, discovered at connect. So the declaration is built per request
// from the live tool set, and a module whose plugin is not connected declares
// zero commands rather than failing. Zero is the honest answer: the plugin is
// not there, so none of its commands are callable.
const (
	commandsRequestMagic  uint32 = 0x444d4344 // "DCMD"
	commandsResponseMagic uint32 = 0x524d4344 // "DCMR"
	commandsRequestLen           = 8

	// ADMISSION.
	//
	// A plugin module does NOT start its plugin on its own. Starting it runs
	// third-party code, and the OSV malware gate that has always guarded an
	// aimee.yaml-declared MCP server lives in C, with the config, the advisory
	// cache and the audit table. Re-implementing that policy in Go would be the
	// dangerous kind of duplicate: it would pass tests while enforcing something
	// subtly different on the path that runs untrusted code.
	//
	// So control is inverted. The module reports the command it WANTS to run and
	// waits; the daemon runs the existing gate and answers. Until it answers,
	// nothing is spawned.
	//
	//	response DCMP | version | argv_len u32 | argv        (pending admission)
	//	         argv is NUL-separated, no trailing NUL
	//	request  DCMD | version | verdict u32 | argv_sha256[32]
	//
	// The hash binds the verdict to the exact argv that was scanned. Without it a
	// module could report a benign command, collect an admit, and then spawn
	// something else -- the gate would have scanned a string that never ran.
	commandsPendingMagic uint32 = 0x504d4344 // "DCMP"
	admitRequestLen             = 8 + 4 + 32

	// AdmitVerdict values. Zero is deliberately not a verdict: a zeroed buffer
	// must not read as "admitted".
	AdmitAllow  uint32 = 1
	AdmitRefuse uint32 = 2
)

// Surface bits. These MUST match aimee_surface_t in src/headers/command_registry.h.
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

// pluginSurfaces is what a plugin tool is exposed on.
//
// MCP is included because reaching plugin tools from an agent is the point, and
// the registry REFUSES an MCP command with no CLI route (command_registry.c:68),
// so CLI comes with it -- correctly: a capability an operator cannot invoke by
// hand is one they cannot debug.
const pluginSurfaces = SurfaceCLI | SurfaceRPC | SurfaceMCP | SurfaceACP

// pluginVisibility keeps plugin tools OUT of the prominent tools/list.
//
// command_registry.h records the measurement behind that list being deliberately
// small: it is a per-session tax on every client. With the target shape of ~15
// plugin modules each advertising its own tools, defaulting to prominent would
// grow that payload without bound and push the core tools down. Discoverable
// keeps them fully callable through find_tools/describe_tool/call_tool.
const pluginVisibility = MCPDiscoverable

// Permission is what an instance is allowed to do, mirroring plugin_permission_t
// in src/headers/plugin.h so the two vocabularies cannot drift.
//
// NOTE this is NOT parity with anything: aimee.yaml-declared MCP clients have no
// per-call permission enforcement at all — plugin_permission_t is parsed and
// stored and never checked. This is a new control, and it is worth having
// because a plugin module's whole job is running someone else's tools.
type Permission int

const (
	PermRead Permission = iota
	PermWrite
	PermExecute
	PermDangerous
)

// ParsePermission mirrors plugin_permission_from_str(): anything unrecognised
// is READ, the least privilege, so a typo cannot widen what a plugin may do.
func ParsePermission(s string) Permission {
	switch s {
	case "write":
		return PermWrite
	case "execute":
		return PermExecute
	case "dangerous":
		return PermDangerous
	default:
		return PermRead
	}
}

func (p Permission) String() string {
	switch p {
	case PermWrite:
		return "write"
	case PermExecute:
		return "execute"
	case PermDangerous:
		return "dangerous"
	default:
		return "read"
	}
}

// toolPermission infers what a tool needs from its MCP annotations.
//
// MCP tools carry optional hints -- readOnlyHint, destructiveHint -- and they
// are the only machine-readable signal about what a tool does. A tool that
// declares nothing is treated as `write`: assuming `read` would let every
// unannotated tool through a read-only ceiling, which is the wrong direction to
// be wrong in.
func toolPermission(t Tool) Permission {
	var ann struct {
		ReadOnly    *bool `json:"readOnlyHint"`
		Destructive *bool `json:"destructiveHint"`
	}
	if len(t.Annotations) > 0 {
		_ = json.Unmarshal(t.Annotations, &ann)
	}
	if ann.Destructive != nil && *ann.Destructive {
		return PermDangerous
	}
	if ann.ReadOnly != nil && *ann.ReadOnly {
		return PermRead
	}
	return PermWrite
}

// Command is one declared verb, mirroring memory.Command.
type Command struct {
	Group      string
	Verb       string
	Summary    string
	Surfaces   uint32
	Visibility uint32
}

// normalizeName maps a plugin-supplied name onto the registry's grammar.
//
// cmd_name_ok() in src/command_registry.c accepts only [a-z0-9_]. Real MCP tool
// names routinely violate that -- "search-issues", "createPullRequest",
// "github.search" -- and a registration that fails is a tool that is silently
// unreachable from every surface, which is precisely the failure the registry
// exists to prevent. So names are folded rather than passed through:
//
//   - upper-case letters lower-case (createPullRequest -> createpullrequest)
//   - any run of characters outside [a-z0-9] collapses to a single underscore
//   - leading and trailing underscores are trimmed
//
// Returns "" when nothing usable survives; the caller reports that tool rather
// than registering a nameless command.
func normalizeName(name string) string {
	var b strings.Builder
	b.Grow(len(name))
	pendingSep := false
	for _, r := range name {
		switch {
		case r >= 'A' && r <= 'Z':
			r = r - 'A' + 'a'
			fallthrough
		case (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9'):
			if pendingSep && b.Len() > 0 {
				b.WriteByte('_')
			}
			pendingSep = false
			b.WriteRune(r)
		default:
			// Collapse a run of separators; do not emit until a real
			// character follows, which trims the trailing case for free.
			pendingSep = true
		}
	}
	return b.String()
}

// BuildCommands projects a plugin's tools onto registry commands under group.
//
// Returns the commands plus the names of tools that could not be represented,
// so the caller can report them. A tool is skipped when its name normalizes to
// nothing, or when it collides with an already-projected verb: the registry
// treats a duplicate (group, verb) as an error rather than an overwrite, and
// silently dropping one of two colliding tools would make which one answers
// depend on tools/list ordering.
func BuildCommands(group string, tools []Tool, ceiling Permission) (commands []Command, skipped []string) {
	group = normalizeName(group)
	if group == "" {
		// Every command needs a group; without one none of them can register.
		for _, t := range tools {
			skipped = append(skipped, t.Name)
		}
		return nil, skipped
	}
	seen := make(map[string]struct{}, len(tools))
	for _, t := range tools {
		verb := normalizeName(t.Name)
		if verb == "" {
			skipped = append(skipped, t.Name)
			continue
		}
		if _, dup := seen[verb]; dup {
			skipped = append(skipped, t.Name)
			continue
		}
		// A tool that needs more than this instance was granted is not declared
		// at all. Declaring it and refusing at call time would advertise a
		// capability that always fails, which reads as a broken tool rather than
		// a withheld one.
		if toolPermission(t) > ceiling {
			skipped = append(skipped, t.Name)
			continue
		}
		seen[verb] = struct{}{}
		commands = append(commands, Command{
			Group:      group,
			Verb:       verb,
			Summary:    summarize(t.Description),
			Surfaces:   pluginSurfaces,
			Visibility: pluginVisibility,
		})
	}
	return commands, skipped
}

// summaryMax bounds a declared summary. The length prefix on the wire is a
// u16, and a plugin's description can be a page of markdown; truncating here
// keeps one oversized description from being unrepresentable on the wire.
const summaryMax = 400

// summarize flattens a description to a single bounded line. Newlines are
// replaced rather than kept: the summary lands in CLI help and tools/list,
// both of which are line-oriented.
func summarize(description string) string {
	s := strings.TrimSpace(strings.NewReplacer("\r", " ", "\n", " ", "\t", " ").Replace(description))
	for strings.Contains(s, "  ") {
		s = strings.ReplaceAll(s, "  ", " ")
	}
	if len(s) > summaryMax {
		// Cut on a rune boundary; a split multi-byte rune would produce an
		// invalid UTF-8 summary in every surface that renders it.
		cut := summaryMax
		for cut > 0 && !utf8Boundary(s, cut) {
			cut--
		}
		s = strings.TrimSpace(s[:cut])
	}
	return s
}

// utf8Boundary reports whether index i in s starts a new rune.
func utf8Boundary(s string, i int) bool {
	if i <= 0 || i >= len(s) {
		return true
	}
	return s[i]&0xC0 != 0x80
}

// EncodeCommands serialises commands in the DCMR response format.
func EncodeCommands(commands []Command) []byte {
	out := make([]byte, 0, 12+len(commands)*64)
	var hdr [12]byte
	binary.LittleEndian.PutUint32(hdr[0:4], commandsResponseMagic)
	binary.LittleEndian.PutUint32(hdr[4:8], wireVersion)
	binary.LittleEndian.PutUint32(hdr[8:12], uint32(len(commands)))
	out = append(out, hdr[:]...)

	for _, c := range commands {
		var rec [16]byte
		binary.LittleEndian.PutUint32(rec[0:4], c.Surfaces)
		binary.LittleEndian.PutUint32(rec[4:8], c.Visibility)
		binary.LittleEndian.PutUint16(rec[8:10], uint16(len(c.Group)))
		binary.LittleEndian.PutUint16(rec[10:12], uint16(len(c.Verb)))
		binary.LittleEndian.PutUint16(rec[12:14], uint16(len(c.Summary)))
		binary.LittleEndian.PutUint16(rec[14:16], 0) // pad, keeps records 4-aligned
		out = append(out, rec[:]...)
		out = append(out, c.Group...)
		out = append(out, c.Verb...)
		out = append(out, c.Summary...)
	}
	return out
}

// validDeclareRequest checks the fixed DCMD request envelope.
func validDeclareRequest(request []byte) bool {
	return len(request) >= commandsRequestLen &&
		binary.LittleEndian.Uint32(request[0:4]) == commandsRequestMagic &&
		binary.LittleEndian.Uint32(request[4:8]) == wireVersion
}

// ArgvDigest is the canonical hash of a command line, computed identically in
// Go and in C (src/module_commands.c). NUL-separated with no trailing
// separator: a separator that can appear inside an argument would let two
// different command lines hash the same, and NUL cannot appear in an argv
// element.
func ArgvDigest(argv []string) [32]byte {
	return sha256.Sum256([]byte(strings.Join(argv, "\x00")))
}

// EncodePending builds the DCMP response asking for an admission verdict.
func EncodePending(argv []string) []byte {
	joined := strings.Join(argv, "\x00")
	out := make([]byte, 12+len(joined))
	binary.LittleEndian.PutUint32(out[0:4], commandsPendingMagic)
	binary.LittleEndian.PutUint32(out[4:8], wireVersion)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(joined)))
	copy(out[12:], joined)
	return out
}

// EncodeAdmitRequest builds a declare request carrying an admission verdict.
// Exported for the callers that drive this stage and for conformance tests.
func EncodeAdmitRequest(verdict uint32, digest [32]byte) []byte {
	out := make([]byte, admitRequestLen)
	binary.LittleEndian.PutUint32(out[0:4], commandsRequestMagic)
	binary.LittleEndian.PutUint32(out[4:8], wireVersion)
	binary.LittleEndian.PutUint32(out[8:12], verdict)
	copy(out[12:], digest[:])
	return out
}

// parseAdmission reads an admission record off a declare request. ok is false
// when the request is a plain declare with no verdict attached.
func parseAdmission(request []byte) (verdict uint32, digest [32]byte, ok bool) {
	if len(request) < admitRequestLen {
		return 0, digest, false
	}
	verdict = binary.LittleEndian.Uint32(request[8:12])
	copy(digest[:], request[12:44])
	return verdict, digest, true
}

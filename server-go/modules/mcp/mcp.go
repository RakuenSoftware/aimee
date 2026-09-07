// Package mcp implements a plugin module: one module process hosting exactly
// one MCP server.
//
// ONE PLUGIN PER MODULE is the governing rule, not a default. The module
// instance IS the scope: its bus identity, its failure domain, its command
// group, and its plugin's lifetime are all the same boundary. A module whose
// plugin dies has lost its only job -- it declares zero commands and answers
// CapabilityAbsent, and nothing else in the deployment is affected. That is why
// this package holds a single *Client and has no registry: a registry inside the
// module would put several plugins back into one failure domain, which is the
// arrangement this replaces.
//
// The module declares its plugin's tools into THE command registry
// (src/headers/command_registry.h) over the event bus, using the same
// declaration contract the memory module answers. Everything downstream -- CLI,
// v1 RPC, MCP tools/list, ACP -- routes from that one declaration.
package mcp

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// EVENT KINDS ARE PER-INSTANCE, NOT PER-PACKAGE.
//
// bus_host_serve_kind() (core/event_bus/bus_route.c:109) binds one event kind to
// exactly ONE serving slot and REFUSES a second module that tries to serve the
// same kind. So package-level EventInvoke/EventDeclareCommands constants would
// mean the first MCP instance to attach wins and every other one is rejected --
// which defeats the whole point of running ten of them.
//
// Each instance therefore gets its own kinds, and they are derived from its
// principal ref by the SAME rule every other module uses:
//
//	kind = 4096 + principal_ref*256 + stage
//
// docs/modules/README.md states that rule, and server-go/aimee, git, roundtable
// and economizer all assert it in their tests. The ref is the single allocation
// authority: refs are already unique per instance, so deriving kinds from the
// ref makes a kind collision structurally impossible rather than something the
// provisioner has to search for and avoid.
//
// An earlier version of this file allocated a *separate* event range starting at
// 11264, reasoning that block 44 was "the next free 256-aligned block after the
// highest currently allocated kind (11010, block 43)". That was wrong, and a
// live run on a real aimee-kb proved it: each ref reserves a whole 256-kind
// block whether or not it uses every stage, and 4096 + 28*256 = 11264 is exactly
// postgres's block. The old range squatted the blocks of postgres (28), db2 (29)
// and db1 (30). It survived testing only because a scratch host runs none of
// them; against a real kb the plugin was refused at attach, and had it attached
// first it would have silently denied postgres instead -- taking db2 health down
// with it. Two allocation authorities for one namespace is the defect; this has
// one.
//
// The ceiling that actually binds is BUS_HOST_MAX_KINDS: 256 kinds interned per
// host, shared with every other module, so at most ~128 instances can attach
// anywhere. The ref band below is wider than that ceiling on purpose, so the
// band is never the binding constraint.
const (
	// PluginRefFirst/PluginRefLimit bound the principal refs reserved for plugin
	// instances. Canonical module refs are 1..30 and are handed out in order, so
	// this band leaves room for many more before it is reached; it is recorded in
	// tests/baselines/modules/canonical-inventory.yaml so no future module is
	// ever assigned into it.
	PluginRefFirst uint32 = 200
	PluginRefLimit uint32 = 456

	// kindOrigin and kindStride are the canonical derivation constants.
	kindOrigin uint32 = 4096
	kindStride uint32 = 256

	// Stage ids are local to a module, so every instance uses the same two.
	StageInvoke          uint32 = 1
	StageDeclareCommands uint32 = 2

	wireVersion uint32 = 1

	invokeRequestMagic   uint32 = 0x51504d43 // "CMPQ"
	invokeResponseMagic  uint32 = 0x53504d43 // "CMPS"
	invokeRequestHeader         = 16
	invokeResponseHeader        = 12

	// verbMax bounds the tool name carried on the wire. Registry verbs are
	// [a-z0-9_] and far shorter; this is a wire sanity bound, not a policy.
	verbMax = 128
	// argsMax bounds one call's argument JSON, well inside the bus body cap.
	argsMax = 1 << 20
)

// callTimeout bounds a single tools/call. A plugin is an external process and
// may hang; the invocation's own deadline still clamps this further via
// ModuleInvocation.Remaining.
const callTimeout = 60 * time.Second

// ErrPluginRef reports a principal ref outside the reserved plugin band.
var ErrPluginRef = errors.New("mcp: invalid plugin principal ref")

// EventKinds returns the (invoke, declare) event kinds for a plugin instance's
// principal ref, by the canonical rule 4096 + ref*256 + stage.
//
// Nothing is searched or supplied: the ref alone determines the kinds, so two
// instances can only share a kind if they share a ref -- which the provisioner
// already prevents, and which the bus grant would reject anyway.
func EventKinds(ref uint32) (invoke, declare uint32, err error) {
	if ref < PluginRefFirst || ref >= PluginRefLimit {
		return 0, 0, fmt.Errorf("%w: %d is outside the reserved plugin band [%d,%d)",
			ErrPluginRef, ref, PluginRefFirst, PluginRefLimit)
	}
	base := kindOrigin + ref*kindStride
	return base + StageInvoke, base + StageDeclareCommands, nil
}

// Module is one plugin module instance.
type Module struct {
	// group is the registry group every command from this plugin lands under,
	// i.e. the instance name ("github" -> `aimee github search`).
	group string

	mu     sync.RWMutex
	client *Client

	// The plugin this instance WANTS to run, and whether it has been cleared.
	//
	// Nothing is spawned until the daemon answers with a verdict: starting the
	// plugin is what executes third-party code, so it happens after the gate,
	// not before it. `decided` latches so a verdict is applied exactly once --
	// re-admitting on every declaration would respawn the plugin each refresh.
	pendingArgv  []string
	pendingCwd   string
	ceiling      Permission
	decided      bool
	refused      bool
	egressClient egress.Client
}

// New builds a module for the named instance with an explicit permission
// ceiling. The plugin may be attached later; until then the module declares no
// commands and refuses calls, which is the honest answer rather than a startup
// failure -- a plugin that is slow to come up must not take its module down.
//
// The ceiling is a parameter rather than a field left at its zero value on
// purpose. Least privilege says the default should be `read`, but a module
// silently defaulted to `read` would drop every unannotated tool without anyone
// choosing that, and the symptom (a plugin that declares nothing) looks
// identical to a broken plugin. Making it explicit means the choice is visible
// at the call site.
func New(group string, ceiling Permission) *Module {
	return &Module{group: normalizeName(group), ceiling: ceiling}
}

// Group returns the normalized registry group for this instance.
func (m *Module) Group() string { return m.group }

func (m *Module) SetEgressClient(client egress.Client) {
	m.mu.Lock()
	m.egressClient = client
	m.mu.Unlock()
}

// Attach binds a connected plugin session. Replacing an existing session closes
// the old one: one plugin per module means the new session supersedes rather
// than joins.
func (m *Module) Attach(client *Client) {
	m.mu.Lock()
	old := m.client
	m.client = client
	m.mu.Unlock()
	if old != nil && old != client {
		_ = old.Close()
	}
}

// Detach drops the plugin session. After this the module declares zero commands.
func (m *Module) Detach() {
	m.mu.Lock()
	old := m.client
	m.client = nil
	m.mu.Unlock()
	if old != nil {
		_ = old.Close()
	}
}

func (m *Module) session() *Client {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.client
}

// Handle dispatches a stage call. It is the bus.ModuleHandler for this module.
func (m *Module) Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	switch invocation.StageID {
	case StageDeclareCommands:
		return m.handleDeclareCommands(invocation, request)
	case StageInvoke:
		return m.handleInvoke(invocation, request)
	}
	return nil, bus.ModuleStatusInvalidRequest
}

// handleDeclareCommands answers with the live plugin's tools projected onto
// registry commands.
//
// Unlike a module with a static command table, this is rebuilt per request:
// the answer must reflect the plugin that is connected NOW. A module with no
// plugin attached declares zero commands and returns OK -- an error here would
// be read as "the module is broken" when the truth is "this plugin currently
// offers nothing", and the registry would keep stale commands that answer
// CapabilityAbsent instead of dropping them.
func (m *Module) handleDeclareCommands(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if !validDeclareRequest(request) {
		// Refuse rather than answer a request we did not understand: a partial
		// command surface is invisible, reading as "aimee cannot do that"
		// rather than as an error.
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	// Apply an admission verdict, if one rode along with this request.
	if verdict, digest, ok := parseAdmission(request); ok {
		if status := m.applyAdmission(invocation.TraceID, verdict, digest); status != bus.ModuleStatusOK {
			return nil, status
		}
	}

	client := m.session()
	if client == nil || !client.Ready() {
		// Still waiting on a verdict for a plugin we have been asked to run:
		// say so, rather than reporting an empty command set that looks like a
		// plugin with nothing to offer.
		m.mu.RLock()
		pending := len(m.pendingArgv) > 0 && !m.decided
		argv := append([]string(nil), m.pendingArgv...)
		m.mu.RUnlock()
		if pending {
			return EncodePending(argv), bus.ModuleStatusOK
		}
		return EncodeCommands(nil), bus.ModuleStatusOK
	}

	// ASK the plugin, do not trust the cache.
	//
	// A dead plugin does not announce itself: the *Client stays non-nil, Ready()
	// stays true, and Tools() keeps returning the tool list from the last
	// successful tools/list. Declaring off that cache kept advertising commands
	// for a plugin that had exited -- found by the process-level e2e, which
	// killed the plugin and watched its commands stay registered. Re-listing is
	// what makes "declare what the plugin offers NOW" actually true, and it
	// catches a hung or broken plugin as well as an exited one.
	//
	// The cost is one round trip per declaration, which the caller already
	// throttles (aimee_module_commands_refresh's TTL).
	if _, err := client.ListTools(); err != nil {
		// The plugin cannot answer, so none of its commands are callable.
		// Detach so every later declaration is honestly empty and the invoke
		// path reports CapabilityAbsent rather than timing out per call.
		// Re-attaching a recovered plugin is a supervisor's job, not this
		// handler's -- it is not implemented, so a plugin that comes back needs
		// its module restarted.
		m.Detach()
		return EncodeCommands(nil), bus.ModuleStatusOK
	}

	m.mu.RLock()
	ceiling := m.ceiling
	m.mu.RUnlock()
	commands, skipped := BuildCommands(m.group, client.Tools(), ceiling)
	if len(skipped) > 0 {
		// Say which tools did not make it and why the set is smaller than the
		// plugin advertises. A silently short command list is indistinguishable
		// from a plugin that simply has fewer tools.
		log.Printf("mcp/%s: %d tool(s) not declared (name collision, unrepresentable name, or above the %s ceiling): %v",
			m.group, len(skipped), ceiling, skipped)
	}
	return EncodeCommands(commands), bus.ModuleStatusOK
}

// SetPending records the plugin this instance should run once admitted.
//
// It deliberately does NOT start anything. `ceiling` is the most this instance
// may do; a tool whose MCP annotations ask for more is never declared.
func (m *Module) SetPending(argv []string, cwd string, ceiling Permission) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.pendingArgv = append([]string(nil), argv...)
	m.pendingCwd = cwd
	m.ceiling = ceiling
	m.decided = false
	m.refused = false
}

// Admitted reports whether a verdict has been applied and allowed the plugin.
func (m *Module) Admitted() bool {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.decided && !m.refused
}

// Refused reports whether the daemon's gate blocked this instance's plugin.
func (m *Module) Refused() bool {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.decided && m.refused
}

// applyAdmission acts on a verdict from the daemon, exactly once.
func (m *Module) applyAdmission(traceID uint64, verdict uint32, digest [32]byte) bus.ModuleStatus {
	m.mu.Lock()
	if m.decided || len(m.pendingArgv) == 0 {
		// Nothing outstanding. A repeat verdict is not an error -- the daemon
		// refreshes on a timer and may still be carrying the last one.
		m.mu.Unlock()
		return bus.ModuleStatusOK
	}
	// The verdict is bound to the argv that was scanned. A mismatch means the
	// two sides disagree about what is being admitted, and the only safe
	// reading of that is "do not run it".
	if ArgvDigest(m.pendingArgv) != digest {
		m.mu.Unlock()
		return bus.ModuleStatusInvalidRequest
	}
	if verdict != AdmitAllow {
		m.decided, m.refused = true, true
		m.mu.Unlock()
		return bus.ModuleStatusOK
	}
	argv := append([]string(nil), m.pendingArgv...)
	cwd := m.pendingCwd
	egressClient := m.egressClient
	m.decided = true
	m.mu.Unlock()

	// Spawn outside the lock: starting a process and completing an MCP
	// handshake is slow, and holding the lock would stall every concurrent
	// invocation on this module.
	client, err := startPlugin(argv, cwd, egressClient, traceID)
	if err != nil {
		// Admitted but unable to start. Not a refusal -- the gate said yes --
		// so leave it decided and command-less rather than pretending it was
		// blocked, which would misattribute the failure in the audit trail.
		return bus.ModuleStatusOK
	}
	m.Attach(client)
	return bus.ModuleStatusOK
}

// SSEPrefix marks a pending "argv" as a remote endpoint rather than a command.
//
// The admission record carries one opaque list of strings, and both transports
// have to travel through it. A prefixed first element keeps that one field doing
// one job: the daemon's OSV gate reads argv[0] as an executable, and a bare URL
// there would be scanned as if it were a package launch.
const SSEPrefix = "sse:"

// startPlugin connects to the plugin and completes the MCP handshake.
//
// Two transports, one seam: `Transport` is what the rest of this package sees,
// so nothing above here knows whether the plugin is a local process or a remote
// endpoint. That parity is what makes the C client's SSE support replaceable
// rather than merely reimplemented for stdio.
func startPlugin(argv []string, cwd string, egressClient egress.Client, traceID uint64) (*Client, error) {
	if len(argv) > 0 && strings.HasPrefix(argv[0], SSEPrefix) {
		endpoint := strings.TrimPrefix(argv[0], SSEPrefix)
		credentialEnv := ""
		if len(argv) > 1 {
			// The token is read from the environment the provisioning owns, not
			// baked into the declared argv: the argv is reported over the bus and
			// logged, and a secret does not belong in either.
			credentialEnv = argv[1]
		}
		transport, err := NewSSETransport(endpoint, credentialEnv, 30*time.Second, egressClient, traceID)
		if err != nil {
			return nil, err
		}
		return handshake(transport)
	}

	transport, err := NewStdioTransport(argv, cwd, os.Stderr)
	if err != nil {
		return nil, err
	}
	return handshake(transport)
}

func handshake(transport Transport) (*Client, error) {
	client := NewClient(transport, 30*time.Second)
	if err := client.Initialize("aimee", "1"); err != nil {
		_ = client.Close()
		return nil, err
	}
	if _, err := client.ListTools(); err != nil {
		_ = client.Close()
		return nil, err
	}
	return client, nil
}

// handleInvoke calls one tool on the plugin.
//
//	request  magic u32 | version u32 | verb_len u16 | pad u16 | args_len u32 | verb | args
//	response magic u32 | version u32 | body_len u32 | body
//
// The body is the plugin's MCP result object, passed through unchanged: this
// module translates addressing, not payloads.
func (m *Module) handleInvoke(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < invokeRequestHeader ||
		binary.LittleEndian.Uint32(request[0:4]) != invokeRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	verbLen := int(binary.LittleEndian.Uint16(request[8:10]))
	argsLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if verbLen == 0 || verbLen > verbMax || argsLen > argsMax ||
		invokeRequestHeader+verbLen+argsLen != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	verb := string(request[invokeRequestHeader : invokeRequestHeader+verbLen])
	args := request[invokeRequestHeader+verbLen:]
	if argsLen > 0 && !json.Valid(args) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	client := m.session()
	if client == nil || !client.Ready() {
		return nil, bus.ModuleStatusCapabilityAbsent
	}

	// Resolve the registry verb back to the plugin's own tool name. The verb
	// arrived through the registry, so it is the NORMALIZED spelling; the
	// plugin only answers to its original one.
	toolName, ok := m.resolveTool(client, verb)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}

	if invocation.Remaining(callTimeout) <= 0 {
		return nil, bus.ModuleStatusDeadlineExceeded
	}
	result, err := client.CallTool(toolName, json.RawMessage(args))
	if err != nil {
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		// The plugin IS attached -- it just failed this call, or the transport
		// broke mid-call. That is Internal, not CapabilityAbsent: the latter
		// says "this module cannot do that at all", which would invite the
		// caller to stop trying rather than retry a transient failure.
		return nil, bus.ModuleStatusInternal
	}

	out := make([]byte, invokeResponseHeader+len(result))
	binary.LittleEndian.PutUint32(out[0:4], invokeResponseMagic)
	binary.LittleEndian.PutUint32(out[4:8], wireVersion)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(result)))
	copy(out[invokeResponseHeader:], result)
	return out, bus.ModuleStatusOK
}

// resolveTool maps a normalized registry verb back to the plugin's tool name.
//
// The mapping is rebuilt from the live tool set rather than cached alongside
// the declaration, so a plugin that changed its tools between declaring and
// being called resolves against what it actually offers now.
func (m *Module) resolveTool(client *Client, verb string) (string, bool) {
	m.mu.RLock()
	ceiling := m.ceiling
	m.mu.RUnlock()

	seen := make(map[string]struct{})
	for _, t := range client.Tools() {
		normalized := normalizeName(t.Name)
		if normalized == "" {
			continue
		}
		if _, dup := seen[normalized]; dup {
			// Collided during projection, so it was never declared and must
			// not become callable by the back door.
			continue
		}
		seen[normalized] = struct{}{}
		if normalized != verb {
			continue
		}
		// Re-check the ceiling here rather than trusting that only declared
		// verbs are ever asked for. The declaration and the invoke arrive on
		// different calls, and a caller holding a verb from before the ceiling
		// was tightened would otherwise still reach the tool.
		if toolPermission(t) > ceiling {
			return "", false
		}
		return t.Name, true
	}
	return "", false
}

// EncodeInvokeRequest builds an invoke request. Exported for the callers that
// drive this stage and for conformance tests.
func EncodeInvokeRequest(verb string, args []byte) []byte {
	out := make([]byte, invokeRequestHeader+len(verb)+len(args))
	binary.LittleEndian.PutUint32(out[0:4], invokeRequestMagic)
	binary.LittleEndian.PutUint32(out[4:8], wireVersion)
	binary.LittleEndian.PutUint16(out[8:10], uint16(len(verb)))
	binary.LittleEndian.PutUint16(out[10:12], 0)
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(args)))
	copy(out[invokeRequestHeader:], verb)
	copy(out[invokeRequestHeader+len(verb):], args)
	return out
}

// EncodeDeclareRequest builds the fixed declaration request envelope.
func EncodeDeclareRequest() []byte {
	out := make([]byte, commandsRequestLen)
	binary.LittleEndian.PutUint32(out[0:4], commandsRequestMagic)
	binary.LittleEndian.PutUint32(out[4:8], wireVersion)
	return out
}

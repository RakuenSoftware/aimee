package mcp

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// --- a fake plugin, so the module is testable without a real MCP server ---

// fakePlugin answers JSON-RPC in memory. It records what it was asked, so a
// test can assert the module addressed the plugin by its ORIGINAL tool name
// rather than the normalized registry spelling.
type fakePlugin struct {
	mu        sync.Mutex
	tools     []Tool
	pending   [][]byte
	calls     []string
	failCall  bool
	closed    bool
	extraJunk bool // emit an unrelated frame before each response
	// outOfOrder makes responses arrive on a delay that DECREASES with request
	// id, so a later request answers first. Real MCP servers are free to do
	// this, and it is what turns a missing round-trip lock from "usually fine"
	// into a lost frame: a caller that reads someone else's response discards
	// it, and its owner then waits out the deadline.
	outOfOrder bool
}

func (f *fakePlugin) Send(frame []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return fmt.Errorf("closed")
	}
	var req rpcRequest
	if err := json.Unmarshal(frame, &req); err != nil {
		return err
	}
	if f.extraJunk {
		// A notification: no id. The client must skip it, not read it as the
		// answer.
		f.pending = append(f.pending, []byte(`{"jsonrpc":"2.0","method":"notifications/progress"}`))
	}
	var resp []byte
	switch req.Method {
	case "initialize":
		resp = []byte(fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"result":{"protocolVersion":"2024-11-05"}}`, req.ID))
	case "tools/list":
		payload, _ := json.Marshal(map[string]any{"tools": f.tools})
		resp = []byte(fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"result":%s}`, req.ID, payload))
	case "tools/call":
		params, _ := json.Marshal(req.Params)
		var p struct {
			Name string `json:"name"`
		}
		_ = json.Unmarshal(params, &p)
		f.calls = append(f.calls, p.Name)
		if f.failCall {
			resp = []byte(fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"error":{"code":-32000,"message":"boom"}}`, req.ID))
		} else {
			resp = []byte(fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"result":{"content":[{"type":"text","text":"ok"}]}}`, req.ID))
		}
	default:
		resp = []byte(fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"error":{"code":-32601,"message":"no method"}}`, req.ID))
	}
	if f.outOfOrder {
		// Answer later requests sooner. Queued off-lock so several requests are
		// genuinely in flight at once.
		delay := time.Duration(40-int(req.ID)*4) * time.Millisecond
		if delay < 0 {
			delay = 0
		}
		go func(frame []byte, d time.Duration) {
			time.Sleep(d)
			f.mu.Lock()
			f.pending = append(f.pending, frame)
			f.mu.Unlock()
		}(resp, delay)
		return nil
	}
	f.pending = append(f.pending, resp)
	return nil
}

func (f *fakePlugin) Recv(timeout time.Duration) ([]byte, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		f.mu.Lock()
		if len(f.pending) > 0 {
			frame := f.pending[0]
			f.pending = f.pending[1:]
			f.mu.Unlock()
			return frame, nil
		}
		f.mu.Unlock()
		time.Sleep(time.Millisecond)
	}
	return nil, ErrTimeout
}

func (f *fakePlugin) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return nil
}

func (f *fakePlugin) callNames() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]string, len(f.calls))
	copy(out, f.calls)
	return out
}

func attached(t *testing.T, group string, plugin *fakePlugin) *Module {
	t.Helper()
	client := NewClient(plugin, 2*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatalf("initialize: %v", err)
	}
	if _, err := client.ListTools(); err != nil {
		t.Fatalf("tools/list: %v", err)
	}
	m := New(group, PermDangerous)
	m.Attach(client)
	return m
}

const defaultTestTimeout = 2 * time.Second

func invocation(stage uint32) bus.ModuleInvocation {
	return bus.ModuleInvocation{StageID: stage}
}

// --- name normalization ---

func TestNormalizeNameFoldsToRegistryGrammar(t *testing.T) {
	// cmd_name_ok() in src/command_registry.c accepts only [a-z0-9_]. Anything
	// else is refused, and a refused registration is a tool unreachable from
	// every surface.
	cases := map[string]string{
		"search_issues":     "search_issues",
		"search-issues":     "search_issues",
		"createPullRequest": "createpullrequest",
		"github.search":     "github_search",
		"a  b":              "a_b",
		"__leading":         "leading",
		"trailing__":        "trailing",
		"read file (utf8)":  "read_file_utf8",
		"tool2":             "tool2",
		"":                  "",
		"---":               "",
		"日本語":               "",
	}
	for in, want := range cases {
		if got := normalizeName(in); got != want {
			t.Errorf("normalizeName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestNormalizedNamesSatisfyRegistryGrammar(t *testing.T) {
	for _, in := range []string{"search-issues", "createPullRequest", "a.b.c", "x  y", "T2"} {
		got := normalizeName(in)
		if got == "" {
			t.Fatalf("normalizeName(%q) produced nothing", in)
		}
		for _, r := range got {
			if !((r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_') {
				t.Errorf("normalizeName(%q) = %q contains %q, outside [a-z0-9_]", in, got, r)
			}
		}
	}
}

// --- command projection ---

func TestBuildCommandsProjectsTools(t *testing.T) {
	tools := []Tool{
		{Name: "search-issues", Description: "Search  issues\nacross repos"},
		{Name: "createPullRequest", Description: "Open a PR"},
	}
	commands, skipped := BuildCommands("github", tools, PermDangerous)
	if len(skipped) != 0 {
		t.Fatalf("unexpected skips: %v", skipped)
	}
	if len(commands) != 2 {
		t.Fatalf("got %d commands, want 2", len(commands))
	}
	if commands[0].Group != "github" || commands[0].Verb != "search_issues" {
		t.Errorf("got %s.%s, want github.search_issues", commands[0].Group, commands[0].Verb)
	}
	// Newlines flattened and runs collapsed: the summary lands in line-oriented
	// surfaces (CLI help, tools/list).
	if commands[0].Summary != "Search issues across repos" {
		t.Errorf("summary = %q", commands[0].Summary)
	}
	// The registry refuses an MCP command with no CLI route
	// (src/command_registry.c:68), so both bits must be present together.
	if commands[0].Surfaces&SurfaceMCP == 0 || commands[0].Surfaces&SurfaceCLI == 0 {
		t.Errorf("surfaces %#x must carry MCP and CLI together", commands[0].Surfaces)
	}
	// Plugin tools stay out of the prominent tools/list: it is a per-session
	// tax and ~15 plugin modules would grow it without bound.
	if commands[0].Visibility != MCPDiscoverable {
		t.Errorf("visibility = %d, want discoverable", commands[0].Visibility)
	}
}

func TestBuildCommandsSkipsCollisionsRatherThanOverwriting(t *testing.T) {
	// Two distinct tool names that normalize to the same verb. The registry
	// treats a duplicate (group, verb) as an error, so silently keeping one
	// would make which tool answers depend on tools/list ordering.
	tools := []Tool{
		{Name: "search-issues"},
		{Name: "search.issues"},
	}
	commands, skipped := BuildCommands("github", tools, PermDangerous)
	if len(commands) != 1 {
		t.Fatalf("got %d commands, want 1", len(commands))
	}
	if len(skipped) != 1 || skipped[0] != "search.issues" {
		t.Fatalf("skipped = %v, want [search.issues]", skipped)
	}
}

func TestBuildCommandsSkipsUnrepresentableNames(t *testing.T) {
	commands, skipped := BuildCommands("github", []Tool{{Name: "---"}, {Name: "ok"}}, PermDangerous)
	if len(commands) != 1 || commands[0].Verb != "ok" {
		t.Fatalf("commands = %+v", commands)
	}
	if len(skipped) != 1 || skipped[0] != "---" {
		t.Fatalf("skipped = %v", skipped)
	}
}

func TestBuildCommandsRefusesEverythingWithoutAGroup(t *testing.T) {
	commands, skipped := BuildCommands("!!!", []Tool{{Name: "ok"}}, PermDangerous)
	if len(commands) != 0 {
		t.Fatalf("commands = %+v, want none without a usable group", commands)
	}
	if len(skipped) != 1 {
		t.Fatalf("skipped = %v", skipped)
	}
}

func TestSummaryTruncatesOnARuneBoundary(t *testing.T) {
	long := strings.Repeat("é", summaryMax)
	got := summarize(long)
	if len(got) > summaryMax {
		t.Fatalf("summary is %d bytes, over the %d cap", len(got), summaryMax)
	}
	// A split multi-byte rune would render as U+FFFD in every surface.
	if !json.Valid([]byte(`"` + got + `"`)) {
		t.Fatalf("summary is not valid UTF-8 after truncation")
	}
}

// --- declaration wire format ---

// decodeCommands mirrors what the C registry decoder must do, so the encoding
// is asserted against an independent reader rather than against itself.
func decodeCommands(t *testing.T, payload []byte) []Command {
	t.Helper()
	if len(payload) < 12 {
		t.Fatalf("payload too short: %d", len(payload))
	}
	if binary.LittleEndian.Uint32(payload[0:4]) != commandsResponseMagic {
		t.Fatalf("bad magic")
	}
	if binary.LittleEndian.Uint32(payload[4:8]) != wireVersion {
		t.Fatalf("bad version")
	}
	count := int(binary.LittleEndian.Uint32(payload[8:12]))
	out := make([]Command, 0, count)
	off := 12
	for i := 0; i < count; i++ {
		if off+16 > len(payload) {
			t.Fatalf("record %d truncated", i)
		}
		var c Command
		c.Surfaces = binary.LittleEndian.Uint32(payload[off : off+4])
		c.Visibility = binary.LittleEndian.Uint32(payload[off+4 : off+8])
		gl := int(binary.LittleEndian.Uint16(payload[off+8 : off+10]))
		vl := int(binary.LittleEndian.Uint16(payload[off+10 : off+12]))
		sl := int(binary.LittleEndian.Uint16(payload[off+12 : off+14]))
		off += 16
		if off+gl+vl+sl > len(payload) {
			t.Fatalf("record %d strings truncated", i)
		}
		c.Group = string(payload[off : off+gl])
		c.Verb = string(payload[off+gl : off+gl+vl])
		c.Summary = string(payload[off+gl+vl : off+gl+vl+sl])
		off += gl + vl + sl
		out = append(out, c)
	}
	if off != len(payload) {
		t.Fatalf("%d trailing bytes after %d records", len(payload)-off, count)
	}
	return out
}

func TestDeclareCommandsRoundTrips(t *testing.T) {
	plugin := &fakePlugin{tools: []Tool{
		{Name: "search-issues", Description: "Search issues"},
		{Name: "create_pr", Description: "Open a PR"},
	}}
	m := attached(t, "github", plugin)

	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	commands := decodeCommands(t, payload)
	if len(commands) != 2 {
		t.Fatalf("got %d commands, want 2", len(commands))
	}
	if commands[0].Group != "github" || commands[0].Verb != "search_issues" {
		t.Errorf("first = %s.%s", commands[0].Group, commands[0].Verb)
	}
	if commands[1].Verb != "create_pr" {
		t.Errorf("second verb = %s", commands[1].Verb)
	}
}

func TestDeclareCommandsRefusesAMalformedRequest(t *testing.T) {
	m := attached(t, "github", &fakePlugin{})
	for _, bad := range [][]byte{
		nil,
		{1, 2, 3},
		append([]byte{0xff, 0xff, 0xff, 0xff}, 0, 0, 0, 1),
	} {
		if _, status := m.Handle(invocation(StageDeclareCommands), bad); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("status for %v = %v, want InvalidRequest", bad, status)
		}
	}
}

func TestDeclareCommandsIsEmptyWithNoPluginAttached(t *testing.T) {
	// A module whose plugin is not connected declares nothing and returns OK.
	// Failing here would read as "the module is broken"; the truth is that none
	// of the plugin's commands are currently callable, and the registry should
	// drop them rather than keep stale entries.
	m := New("github", PermDangerous)
	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if got := decodeCommands(t, payload); len(got) != 0 {
		t.Fatalf("got %d commands, want 0", len(got))
	}
}

func TestDetachWithdrawsCommands(t *testing.T) {
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}}
	m := attached(t, "github", plugin)
	if payload, _ := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest()); len(decodeCommands(t, payload)) != 1 {
		t.Fatal("expected one command while attached")
	}
	m.Detach()
	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got := decodeCommands(t, payload); len(got) != 0 {
		t.Fatalf("got %d commands after detach, want 0", len(got))
	}
}

// --- invocation ---

func TestInvokeAddressesThePluginByItsOriginalName(t *testing.T) {
	// The verb arrives through the registry in its NORMALIZED spelling; the
	// plugin only answers to its own. Getting this backwards makes every
	// non-conforming tool name uncallable.
	plugin := &fakePlugin{tools: []Tool{{Name: "search-issues"}}}
	m := attached(t, "github", plugin)

	body, status := m.Handle(invocation(StageInvoke),
		EncodeInvokeRequest("search_issues", []byte(`{"q":"bug"}`)))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if binary.LittleEndian.Uint32(body[0:4]) != invokeResponseMagic {
		t.Fatalf("bad response magic")
	}
	n := int(binary.LittleEndian.Uint32(body[8:12]))
	if invokeResponseHeader+n != len(body) {
		t.Fatalf("body length %d does not match header %d", len(body), n)
	}
	if !json.Valid(body[invokeResponseHeader:]) {
		t.Fatalf("result body is not JSON")
	}
	if got := plugin.callNames(); len(got) != 1 || got[0] != "search-issues" {
		t.Fatalf("plugin saw %v, want [search-issues]", got)
	}
}

func TestInvokeRefusesAnUndeclaredVerb(t *testing.T) {
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}}
	m := attached(t, "github", plugin)
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("nope", nil)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest", status)
	}
	if got := plugin.callNames(); len(got) != 0 {
		t.Fatalf("plugin was called for an undeclared verb: %v", got)
	}
}

func TestInvokeRefusesAVerbThatCollidedDuringProjection(t *testing.T) {
	// A tool skipped for colliding was never declared, so it must not become
	// callable through the invoke path either.
	plugin := &fakePlugin{tools: []Tool{{Name: "search-issues"}, {Name: "search.issues"}}}
	m := attached(t, "github", plugin)
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("search_issues", nil)); status != bus.ModuleStatusOK {
		t.Fatalf("the surviving tool must still be callable, got %v", status)
	}
	if got := plugin.callNames(); len(got) != 1 || got[0] != "search-issues" {
		t.Fatalf("plugin saw %v, want the first-projected tool only", got)
	}
}

func TestInvokeWithNoPluginIsCapabilityAbsent(t *testing.T) {
	m := New("github", PermDangerous)
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("x", nil)); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("status = %v, want CapabilityAbsent", status)
	}
}

func TestInvokePluginErrorIsInternalNotCapabilityAbsent(t *testing.T) {
	// The plugin is attached and answered -- it just failed. Reporting
	// CapabilityAbsent would tell the caller to stop trying.
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}, failCall: true}
	m := attached(t, "github", plugin)
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("ok", nil)); status != bus.ModuleStatusInternal {
		t.Fatalf("status = %v, want Internal", status)
	}
}

func TestInvokeRefusesMalformedRequests(t *testing.T) {
	m := attached(t, "github", &fakePlugin{tools: []Tool{{Name: "ok"}}})

	short := EncodeInvokeRequest("ok", nil)[:8]
	if _, status := m.Handle(invocation(StageInvoke), short); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("short request accepted")
	}

	// Declared lengths that do not add up to the frame must be refused rather
	// than read past.
	bad := EncodeInvokeRequest("ok", []byte(`{}`))
	binary.LittleEndian.PutUint32(bad[12:16], 4096)
	if _, status := m.Handle(invocation(StageInvoke), bad); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("length-mismatched request accepted")
	}

	// Empty verb.
	empty := EncodeInvokeRequest("", nil)
	if _, status := m.Handle(invocation(StageInvoke), empty); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("empty verb accepted")
	}

	// Non-JSON arguments never reach the plugin.
	junk := EncodeInvokeRequest("ok", []byte(`not json`))
	if _, status := m.Handle(invocation(StageInvoke), junk); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("non-JSON arguments accepted")
	}
}

func TestUnknownStageIsRefused(t *testing.T) {
	m := attached(t, "github", &fakePlugin{})
	if _, status := m.Handle(invocation(99), EncodeDeclareRequest()); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest", status)
	}
}

// --- client behaviour ---

func TestClientSkipsFramesThatAreNotItsResponse(t *testing.T) {
	// MCP servers interleave notifications with responses. Reading one as the
	// result would return the wrong body under load rather than failing.
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}, extraJunk: true}
	client := NewClient(plugin, 2*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatalf("initialize: %v", err)
	}
	tools, err := client.ListTools()
	if err != nil {
		t.Fatalf("tools/list: %v", err)
	}
	if len(tools) != 1 || tools[0].Name != "ok" {
		t.Fatalf("tools = %+v", tools)
	}
}

func TestConcurrentCallsDoNotStealEachOthersResponses(t *testing.T) {
	// The bus allows 16 in-flight invocations per module against one pair of
	// pipes. The plugin here answers OUT OF ORDER, which is what makes the
	// hazard deterministic: without a round-trip lock, a caller reads a frame
	// belonging to another call, discards it for having the wrong id, and that
	// call then waits out its deadline. Removing Client.callMu must fail this.
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}, outOfOrder: true}
	m := attached(t, "github", plugin)

	const n = 8
	var wg sync.WaitGroup
	errs := make(chan bus.ModuleStatus, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("ok", nil))
			errs <- status
		}()
	}
	wg.Wait()
	close(errs)
	for status := range errs {
		if status != bus.ModuleStatusOK {
			t.Fatalf("concurrent call returned %v", status)
		}
	}
	if got := len(plugin.callNames()); got != n {
		t.Fatalf("plugin saw %d calls, want %d", got, n)
	}
}

func TestCancelledInvocationDoesNotReachThePlugin(t *testing.T) {
	plugin := &fakePlugin{tools: []Tool{{Name: "ok"}}}
	m := attached(t, "github", plugin)

	// A deadline already in the past is the cancellation the transport owns.
	inv := bus.ModuleInvocation{StageID: StageInvoke, DeadlineNS: 1}
	if _, status := m.Handle(inv, EncodeInvokeRequest("ok", nil)); status != bus.ModuleStatusCancelled {
		t.Fatalf("status = %v, want Cancelled", status)
	}
	if got := plugin.callNames(); len(got) != 0 {
		t.Fatalf("plugin was called for a cancelled invocation: %v", got)
	}
}

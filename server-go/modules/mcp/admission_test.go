package mcp

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// A plugin module must not start its plugin on its own. Starting it executes
// third-party code, and the OSV malware gate that has always guarded an
// aimee.yaml-declared MCP server lives in C. So the module reports what it wants
// to run and waits; nothing is spawned before a verdict.

func TestPendingPluginIsNotStartedAndIsReported(t *testing.T) {
	m := New("github", PermDangerous)
	// A command that would fail loudly if it were ever actually executed.
	m.SetPending([]string{"/nonexistent/should-never-run", "--x"}, "", PermDangerous)

	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	// DCMP, not DCMR: "I am waiting on a verdict" is a different answer from
	// "I have no commands", and conflating them would hide the gate entirely.
	if got := binary.LittleEndian.Uint32(payload[0:4]); got != commandsPendingMagic {
		t.Fatalf("magic = %#x, want the pending magic %#x", got, commandsPendingMagic)
	}
	argvLen := binary.LittleEndian.Uint32(payload[8:12])
	if int(argvLen)+12 != len(payload) {
		t.Fatalf("argv length %d disagrees with payload %d", argvLen, len(payload))
	}
	if string(payload[12:]) != "/nonexistent/should-never-run\x00--x" {
		t.Fatalf("argv on the wire = %q", string(payload[12:]))
	}
	if m.Admitted() || m.Refused() {
		t.Fatal("no verdict was sent, so the module must be undecided")
	}
}

func TestRefusalStopsThePluginFromEverStarting(t *testing.T) {
	argv := []string{"/nonexistent/should-never-run"}
	m := New("github", PermDangerous)
	m.SetPending(argv, "", PermDangerous)

	_, status := m.Handle(invocation(StageDeclareCommands),
		EncodeAdmitRequest(AdmitRefuse, ArgvDigest(argv)))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !m.Refused() || m.Admitted() {
		t.Fatal("a refusal must latch as refused")
	}
	// After a refusal the module declares nothing and stays that way.
	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got := binary.LittleEndian.Uint32(payload[0:4]); got != commandsResponseMagic {
		t.Fatalf("a decided module must answer DCMR, got %#x", got)
	}
	if got := decodeCommands(t, payload); len(got) != 0 {
		t.Fatalf("a refused plugin declared %d commands", len(got))
	}
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("anything", nil)); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("invoke on a refused instance = %v, want CapabilityAbsent", status)
	}
}

func TestAdmissionMustMatchTheArgvThatWasScanned(t *testing.T) {
	// Without binding the verdict to a hash of the argv, a module could report a
	// benign command, collect an admit, and then spawn something else -- the gate
	// would have scanned a string that never ran.
	m := New("github", PermDangerous)
	m.SetPending([]string{"/nonexistent/real-command"}, "", PermDangerous)

	wrong := ArgvDigest([]string{"/bin/echo", "something-else"})
	_, status := m.Handle(invocation(StageDeclareCommands), EncodeAdmitRequest(AdmitAllow, wrong))
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("status = %v, want InvalidRequest for a mismatched digest", status)
	}
	if m.Admitted() {
		t.Fatal("a mismatched verdict must not admit anything")
	}
}

func TestAZeroedVerdictIsNotAnAdmit(t *testing.T) {
	// A zeroed buffer must not read as "admitted".
	argv := []string{"/nonexistent/should-never-run"}
	m := New("github", PermDangerous)
	m.SetPending(argv, "", PermDangerous)
	if _, status := m.Handle(invocation(StageDeclareCommands),
		EncodeAdmitRequest(0, ArgvDigest(argv))); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if m.Admitted() {
		t.Fatal("verdict 0 was treated as an admit")
	}
	if !m.Refused() {
		t.Fatal("verdict 0 must land as refused, not undecided")
	}
}

func TestAModuleWithNoPendingPluginDeclaresNormally(t *testing.T) {
	// The pending path must not change a module that was handed a live client
	// directly (the ordinary attached case).
	m := attached(t, "github", &fakePlugin{tools: []Tool{{Name: "ok"}}})
	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got := binary.LittleEndian.Uint32(payload[0:4]); got != commandsResponseMagic {
		t.Fatalf("magic = %#x, want DCMR", got)
	}
	if got := decodeCommands(t, payload); len(got) != 1 {
		t.Fatalf("got %d commands, want 1", len(got))
	}
}

// --- permission ceiling ---

func TestParsePermissionMirrorsTheCTaxonomy(t *testing.T) {
	// plugin_permission_from_str() in src/plugin.c: anything unrecognised is
	// READ, so a typo cannot widen what a plugin may do.
	cases := map[string]Permission{
		"read": PermRead, "write": PermWrite, "execute": PermExecute,
		"dangerous": PermDangerous, "": PermRead, "DANGEROUS": PermRead, "nonsense": PermRead,
	}
	for in, want := range cases {
		if got := ParsePermission(in); got != want {
			t.Errorf("ParsePermission(%q) = %v, want %v", in, got, want)
		}
	}
}

func TestToolPermissionReadsMCPAnnotations(t *testing.T) {
	cases := []struct {
		name string
		ann  string
		want Permission
	}{
		{"no annotations", "", PermWrite},
		{"empty annotations", `{}`, PermWrite},
		{"read-only", `{"readOnlyHint":true}`, PermRead},
		{"read-only false", `{"readOnlyHint":false}`, PermWrite},
		{"destructive", `{"destructiveHint":true}`, PermDangerous},
		// Destructive wins: a tool that claims both is the dangerous one.
		{"both", `{"readOnlyHint":true,"destructiveHint":true}`, PermDangerous},
		{"malformed", `not json`, PermWrite},
	}
	for _, c := range cases {
		got := toolPermission(Tool{Name: "t", Annotations: []byte(c.ann)})
		if got != c.want {
			t.Errorf("%s: toolPermission = %v, want %v", c.name, got, c.want)
		}
	}
}

func TestCeilingKeepsOverPrivilegedToolsUndeclared(t *testing.T) {
	tools := []Tool{
		{Name: "safe_read", Annotations: []byte(`{"readOnlyHint":true}`)},
		{Name: "ordinary"},
		{Name: "wipe_everything", Annotations: []byte(`{"destructiveHint":true}`)},
	}

	// A read-only instance gets only the read-only tool. An unannotated tool
	// counts as `write`, deliberately: assuming `read` would let everything
	// through a read-only ceiling.
	commands, skipped := BuildCommands("github", tools, PermRead)
	if len(commands) != 1 || commands[0].Verb != "safe_read" {
		t.Fatalf("read ceiling produced %+v", commands)
	}
	if len(skipped) != 2 {
		t.Fatalf("skipped = %v, want the two over-privileged tools", skipped)
	}

	// A write instance gets the read and write tools, never the destructive one.
	commands, _ = BuildCommands("github", tools, PermWrite)
	if len(commands) != 2 {
		t.Fatalf("write ceiling produced %+v", commands)
	}
	for _, c := range commands {
		if c.Verb == "wipe_everything" {
			t.Fatal("a destructive tool passed a write ceiling")
		}
	}

	// Only a dangerous instance sees all three.
	commands, _ = BuildCommands("github", tools, PermDangerous)
	if len(commands) != 3 {
		t.Fatalf("dangerous ceiling produced %+v", commands)
	}
}

func TestAnUndeclaredOverPrivilegedToolIsAlsoUncallable(t *testing.T) {
	// Defence in depth: a tool held back by the ceiling must not be reachable
	// through the invoke path either.
	plugin := &fakePlugin{tools: []Tool{
		{Name: "wipe", Annotations: []byte(`{"destructiveHint":true}`)},
	}}
	client := NewClient(plugin, defaultTestTimeout)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatal(err)
	}
	if _, err := client.ListTools(); err != nil {
		t.Fatal(err)
	}
	m := New("github", PermRead)
	m.Attach(client)

	payload, _ := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if got := decodeCommands(t, payload); len(got) != 0 {
		t.Fatalf("a destructive tool was declared under a read ceiling: %+v", got)
	}
	if _, status := m.Handle(invocation(StageInvoke), EncodeInvokeRequest("wipe", nil)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("invoke = %v, want InvalidRequest for a tool above the ceiling", status)
	}
	if got := plugin.callNames(); len(got) != 0 {
		t.Fatalf("the plugin was called for an over-privileged tool: %v", got)
	}
}

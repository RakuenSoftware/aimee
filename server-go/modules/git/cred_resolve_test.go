package git

import (
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// The hash keys the mirror tier's server-side trees, and C derives it too
// (fnv1a_hex8 in workspace_mirror.c). A disagreement does not fail loudly: it
// reports "no workspace owns this path", the caller concludes there is no
// credential to inject, and git runs bare. So pin exact values rather than
// re-deriving them here — a test that recomputes the hash the same way agrees
// with itself no matter which side drifted.
//
// The expected values were PRINTED BY THE C FUNCTION, not worked out by hand:
// compile fnv1a_hex8 from src/modules/workspace/workspace_mirror.c and feed it
// these strings. The first three are also the published FNV-1a 32-bit vectors,
// so a wrong constant here shows up as disagreeing with the standard rather
// than only with us. (Hand-deriving the fourth is how the first draft of this
// test failed against a correct implementation.)
func TestFNV1aHex8MatchesTheCDerivation(t *testing.T) {
	for _, tc := range []struct{ in, want string }{
		{"", "811c9dc5"},
		{"a", "e40c292c"},
		{"foobar", "bf9cf968"},
		{"/home/someone/proj/.aimee/worktrees/076a64f1-deadbeef/main", "1f663ffc"},
		{"/home/someone/proj/main", "691bf9f4"},
		{"/home/someone/other", "f4965e2b"},
	} {
		if got := fnv1aHex8(tc.in); got != tc.want {
			t.Fatalf("fnv1aHex8(%q) = %s, want %s", tc.in, got, tc.want)
		}
	}
}

func TestRunsOnServerIsEverythingButDetached(t *testing.T) {
	for _, kind := range []uint32{ProviderShared, ProviderMirror, ProviderContainer} {
		if !runsOnServer(kind) {
			t.Fatalf("provider kind %d must run on the server", kind)
		}
	}
	if runsOnServer(ProviderDetached) {
		t.Fatal("a detached workspace marshals git to its client and must not run here")
	}
	// An unknown kind must default to the credentialed server path, not fall
	// into the bare one: that default is the whole point of the negative form.
	if !runsOnServer(99) {
		t.Fatal("an unrecognised provider kind must default to running on the server")
	}
}

// The bug: a mirror workspace is registered under the CLIENT's path, which does
// not exist on this server, while git runs in the reconstruction under
// <base>/<hash(root)>/. Matching only the registered root answered "no
// workspace" about the live checkout, so no credential was injected.
func TestWorkspaceForPathCoversTheServerSideTree(t *testing.T) {
	const root = "/home/someone/proj/.aimee/worktrees/076a64f1-deadbeef/main"
	const base = "/var/lib/aimee-workspaces"
	roots := []string{"/srv/other", root}
	hashed := base + "/" + fnv1aHex8(root)

	for _, cwd := range []string{
		root,               // in place
		root + "/src/x.c",  // beneath the registered root
		hashed + "/work",   // the plain reconstruction
		hashed + "/mirror", // the bare mirror
		// Generation-qualified: the live worktree carries a generation and a
		// digest, so an exact "<base>/<hash>/work" comparison misses it.
		hashed + "/work-1-7df53596872a77b213ab54022a810bc7",
		hashed + "/work-2-33265f6f97729ac86a284ce69a6c5f45/src/x.c",
	} {
		if got := workspaceForPath(cwd, base, roots); got != root {
			t.Fatalf("workspaceForPath(%q) = %q, want the registered root %q", cwd, got, root)
		}
	}
}

// A containment test that says yes too often hands one workspace's credential to
// work running outside it, so the misses matter as much as the hits.
func TestWorkspaceForPathRejectsPathsItDoesNotOwn(t *testing.T) {
	const root = "/home/someone/proj/main"
	const base = "/var/lib/aimee-workspaces"
	roots := []string{root}

	for _, cwd := range []string{
		"/var/tmp/not-a-workspace",
		root + "-sibling", // shares a textual prefix only
		base + "/" + fnv1aHex8("/home/someone/other") + "/work", // another workspace's tree
		"",
	} {
		if got := workspaceForPath(cwd, base, roots); got != "" {
			t.Fatalf("workspaceForPath(%q) = %q, want no owner", cwd, got)
		}
	}

	// With no mirror base resolved, the hashed arm must simply not fire rather
	// than match everything under "/".
	if got := workspaceForPath(base+"/"+fnv1aHex8(root)+"/work", "", roots); got != "" {
		t.Fatalf("with no mirror base the reconstruction must not resolve, got %q", got)
	}
}

func TestHandleCredResolveAnswersBothQuestions(t *testing.T) {
	const root = "/home/someone/proj/main"
	const base = "/var/lib/aimee-workspaces"
	t.Setenv("AIMEE_WORKSPACES_DIR", base)
	live := base + "/" + fnv1aHex8(root) + "/work-1-abc"

	request, err := json.Marshal(CredResolveRequest{
		Cwd: live, ProviderKind: ProviderMirror, AimeeHome: "/unused", Workspaces: []string{root},
	})
	if err != nil {
		t.Fatal(err)
	}
	body, status := Handle(bus.ModuleInvocation{StageID: StageCredResolve}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	var decoded CredResolveResponse
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if !decoded.RunsOnServer {
		t.Fatal("a mirror workspace runs git on the server")
	}
	if decoded.Workspace != root {
		t.Fatalf("workspace = %q, want the registered root %q", decoded.Workspace, root)
	}

	// Detached: no server-side run, and no workspace is reported — the client
	// holds its own credentials, and naming a workspace here would invite the
	// caller to inject one anyway.
	request, err = json.Marshal(CredResolveRequest{
		Cwd: root, ProviderKind: ProviderDetached, AimeeHome: "/unused", Workspaces: []string{root},
	})
	if err != nil {
		t.Fatal(err)
	}
	body, status = Handle(bus.ModuleInvocation{StageID: StageCredResolve}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	decoded = CredResolveResponse{}
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.RunsOnServer || decoded.Workspace != "" {
		t.Fatalf("detached must not run here nor name a workspace, got %+v", decoded)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageCredResolve}, []byte("{")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed JSON must be rejected, got %v", status)
	}
}

// mirrorBase must agree with workspace_mirror_base(): the env override wins only
// when it names an absolute path, and otherwise the home decides. Getting this
// wrong does not fail loudly — it points the hashed lookup at a directory that
// holds nothing, so every mirror workspace silently reports "no owner" again.
func TestMirrorBaseMatchesTheCResolution(t *testing.T) {
	t.Setenv("AIMEE_WORKSPACES_DIR", "/mnt/vol/workspaces")
	if got := mirrorBase("/home/u/.config/aimee"); got != "/mnt/vol/workspaces" {
		t.Fatalf("an absolute env override must win, got %q", got)
	}
	// A relative value is not a path the C side would accept either.
	t.Setenv("AIMEE_WORKSPACES_DIR", "relative/dir")
	if got := mirrorBase("/home/u/.config/aimee"); got != "/home/u/.config/aimee/workspaces" {
		t.Fatalf("a relative override must be ignored, got %q", got)
	}
	t.Setenv("AIMEE_WORKSPACES_DIR", "")
	if got := mirrorBase("/home/u/.config/aimee"); got != "/home/u/.config/aimee/workspaces" {
		t.Fatalf("base = %q, want the home-derived path", got)
	}
	if got := mirrorBase(""); got != "" {
		t.Fatalf("with no home there is no base, got %q", got)
	}
}

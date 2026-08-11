package delegates

import (
	"strings"
	"testing"
)

func argsFor(t *testing.T, req SandboxRequest) []string {
	t.Helper()
	spec, err := BuildSandboxSpec(req)
	if err != nil {
		t.Fatalf("build spec: %v", err)
	}
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec:          spec,
		ContainerName: "aimee-delegate-1",
		Image:         "ubuntu:22.04",
		WorkDir:       req.Worktree,
	})
	if err != nil {
		t.Fatalf("build args: %v", err)
	}
	return args
}

func hasFlagValue(args []string, flag, value string) bool {
	for i := 0; i+1 < len(args); i++ {
		if args[i] == flag && args[i+1] == value {
			return true
		}
	}
	return false
}

// The one flag the whole design rests on. Never conditional, never
// configurable.
func TestDockerArgsAlwaysDisablesNetwork(t *testing.T) {
	for _, req := range []SandboxRequest{writeReq(), readReq()} {
		args := argsFor(t, req)
		if !hasFlagValue(args, "--network", "none") {
			t.Errorf("--network none missing from %v", args)
		}
	}
}

// A read-only role's workspace must carry :ro all the way to the command, or
// the enforcement was only ever a comment.
func TestDockerArgsReadOnlyRoleGetsReadOnlyBind(t *testing.T) {
	args := argsFor(t, readReq())
	if !hasFlagValue(args, "-v", "/srv/repo:/srv/repo:ro") {
		t.Errorf("the parent worktree was not bound read-only: %v", args)
	}
	// Only the control socket may be writable.
	for i := 0; i+1 < len(args); i++ {
		if args[i] != "-v" {
			continue
		}
		bind := args[i+1]
		if strings.HasSuffix(bind, ":ro") {
			continue
		}
		if !strings.Contains(bind, "aimee.sock") {
			t.Errorf("writable bind for a read-only role: %q", bind)
		}
	}
}

// A write role gets the tree readable and only its worktree writable.
func TestDockerArgsWriteRoleMountModes(t *testing.T) {
	args := argsFor(t, writeReq())
	if !hasFlagValue(args, "-v", "/srv/repo:/srv/repo:ro") {
		t.Errorf("repo not bound read-only: %v", args)
	}
	wt := "/srv/repo/.aimee/worktrees/w1/main"
	if !hasFlagValue(args, "-v", wt+":"+wt) {
		t.Errorf("worktree not bound writable: %v", args)
	}
	gd := "/srv/repo/.git/worktrees/w1"
	if !hasFlagValue(args, "-v", gd+":"+gd) {
		t.Errorf("git dir not bound writable: %v", args)
	}
}

// The image is the last thing before the command, or docker parses a flag as
// the image.
func TestDockerArgsImageAndCommandOrdering(t *testing.T) {
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatalf("build spec: %v", err)
	}
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec: spec, ContainerName: "c1", Image: "ubuntu:22.04",
		Command: []string{"sleep", "infinity"},
	})
	if err != nil {
		t.Fatalf("build args: %v", err)
	}
	for i, a := range args {
		if a == "ubuntu:22.04" {
			if i+2 >= len(args) || args[i+1] != "sleep" || args[i+2] != "infinity" {
				t.Errorf("command does not follow the image: %v", args)
			}
			return
		}
	}
	t.Errorf("image missing from %v", args)
}

// The spec may have travelled by the time it gets here, and a spec that lost
// its guarantees produces a delegate with a network.
func TestDockerArgsRevalidatesTheSpec(t *testing.T) {
	broken := SandboxSpec{
		ReadOnly: true,
		Mounts:   []SandboxMount{{Source: "/srv/repo", Target: "/srv/repo"}},
	}
	if _, err := DockerCreateArgs(DockerCreateRequest{
		Spec: broken, ContainerName: "c1", Image: "ubuntu:22.04",
	}); err == nil {
		t.Error("a spec with a writable workspace mount for a read-only role was rendered")
	}

	sock := SandboxSpec{Mounts: []SandboxMount{{
		Source: "/var/run/docker.sock", Target: "/var/run/docker.sock",
		Kind: SandboxControlSocket,
	}}}
	if _, err := DockerCreateArgs(DockerCreateRequest{
		Spec: sock, ContainerName: "c1", Image: "ubuntu:22.04",
	}); err == nil {
		t.Error("the docker socket was rendered into a create command")
	}
}

func TestDockerArgsRejectsHostileImageReference(t *testing.T) {
	spec, _ := BuildSandboxSpec(writeReq())
	for _, image := range []string{"", "ubuntu; rm -rf /", "$(id)", "ubuntu\nRUN evil"} {
		if _, err := DockerCreateArgs(DockerCreateRequest{
			Spec: spec, ContainerName: "c1", Image: image,
		}); err == nil {
			t.Errorf("accepted hostile image %q", image)
		}
	}
}

// Docker resolves bind sources in the DAEMON's namespace. An untranslated path
// makes a real directory look absent, and docker then creates an empty one --
// the delegate gets an empty mount and simply appears to find nothing.
func TestTranslateMountPath(t *testing.T) {
	table := "/srv/repo\t/host/data/repo\n/run/aimee\t/host/run/aimee\n"
	cases := []struct{ in, want string }{
		{"/srv/repo", "/host/data/repo"},
		{"/srv/repo/sub/file.c", "/host/data/repo/sub/file.c"},
		{"/run/aimee/server.sock", "/host/run/aimee/server.sock"},
		// No mapping: unchanged, which is right for a host-native process.
		{"/elsewhere/path", "/elsewhere/path"},
	}
	for _, c := range cases {
		if got := TranslateMountPath(c.in, table); got != c.want {
			t.Errorf("%q -> %q, want %q", c.in, got, c.want)
		}
	}
}

// Whole components only: /data must not match /database.
func TestTranslateMountPathMatchesWholeComponents(t *testing.T) {
	table := "/data\t/host/data\n"
	if got := TranslateMountPath("/database/file", table); got != "/database/file" {
		t.Errorf("/database/file was rewritten by a /data mount: %q", got)
	}
	if got := TranslateMountPath("/data/file", table); got != "/host/data/file" {
		t.Errorf("/data/file -> %q, want /host/data/file", got)
	}
}

// The longest destination wins, so a nested mount beats its parent.
func TestTranslateMountPathLongestMatchWins(t *testing.T) {
	table := "/srv\t/host/srv\n/srv/repo\t/host/elsewhere/repo\n"
	if got := TranslateMountPath("/srv/repo/x", table); got != "/host/elsewhere/repo/x" {
		t.Errorf("nested mount lost to its parent: %q", got)
	}
}

// A root mount is the degenerate case and must not double the separator.
func TestTranslateMountPathRootMount(t *testing.T) {
	if got := TranslateMountPath("/srv/repo", "/\t/host\n"); got != "/host/srv/repo" {
		t.Errorf("root mount -> %q, want /host/srv/repo", got)
	}
}

// Garbage in the table must not corrupt a path.
func TestTranslateMountPathIgnoresMalformedRows(t *testing.T) {
	table := "no-tab-here\n\trelative\tsource\n/srv/repo\t/host/repo\n"
	if got := TranslateMountPath("/srv/repo/x", table); got != "/host/repo/x" {
		t.Errorf("malformed rows disturbed the match: %q", got)
	}
	if got := TranslateMountPath("relative/path", table); got != "relative/path" {
		t.Errorf("a relative path was rewritten: %q", got)
	}
}

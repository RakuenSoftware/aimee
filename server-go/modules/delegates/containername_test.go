package delegates

import (
	"strings"
	"testing"
)

func specWithWorkspace(source string) SandboxSpec {
	return SandboxSpec{Mounts: []SandboxMount{
		{Source: source, Target: "/repo", Kind: SandboxWorkspace},
	}}
}

// Docker container names allow [a-zA-Z0-9_.-]. Anything else becomes '_', and
// the prefix guarantees the leading character is alphanumeric.
func TestContainerNameSanitizesTheTaskID(t *testing.T) {
	name, err := ContainerName("foo bar:baz/qux.v1-2", SandboxSpec{}, "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if !strings.HasPrefix(name, containerNamePrefix) {
		t.Errorf("name %q lost its prefix", name)
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if !isAlnum(c) && c != '_' && c != '.' && c != '-' {
			t.Errorf("name %q contains %q, which docker will not accept", name, c)
		}
	}
	if !strings.Contains(name, "foo_bar_baz_qux.v1-2") {
		t.Errorf("name %q did not sanitize as expected", name)
	}
}

func TestContainerNameRequiresATaskID(t *testing.T) {
	for _, id := range []string{"", "   "} {
		if _, err := ContainerName(id, SandboxSpec{}, ""); err == nil {
			t.Errorf("task id %q should be refused", id)
		}
	}
}

// The reason this rule exists: `docker start` resumes by name, so a task id
// reused against a DIFFERENT workspace must not resume the old container. It
// would run in the previous tree with nothing to say so.
func TestContainerNameChangesWithTheMounts(t *testing.T) {
	a, err := ContainerName("task-1", specWithWorkspace("/repo-a"), "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	b, err := ContainerName("task-1", specWithWorkspace("/repo-b"), "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if a == b {
		t.Errorf("the same name %q was produced for two different workspaces", a)
	}
}

// Same task, same mounts: the name must be stable, or resume never works at all.
func TestContainerNameIsStableForTheSameMounts(t *testing.T) {
	a, _ := ContainerName("task-1", specWithWorkspace("/repo"), "")
	b, _ := ContainerName("task-1", specWithWorkspace("/repo"), "")
	if a != b {
		t.Errorf("name is not stable: %q vs %q", a, b)
	}
}

// The mount mode is part of the mount. A tree mounted read-only is not the same
// container as the same tree mounted writable.
func TestContainerNameChangesWithTheMountMode(t *testing.T) {
	rw := SandboxSpec{Mounts: []SandboxMount{
		{Source: "/repo", Target: "/repo", Kind: SandboxWorkspace},
	}}
	ro := SandboxSpec{Mounts: []SandboxMount{
		{Source: "/repo", Target: "/repo", ReadOnly: true, Kind: SandboxWorkspace},
	}}
	a, _ := ContainerName("task-1", rw, "")
	b, _ := ContainerName("task-1", ro, "")
	if a == b {
		t.Error("read-only and writable mounts produced the same container name")
	}
}

// The fingerprint follows the TRANSLATED source, because that is what is
// actually bound. Fingerprinting the untranslated path would let two different
// host trees share a name.
func TestContainerNameFollowsTheTranslatedSource(t *testing.T) {
	spec := specWithWorkspace("/repo")
	plain, _ := ContainerName("task-1", spec, "")
	translated, _ := ContainerName("task-1", spec, "/repo\t/host/checkout")
	if plain == translated {
		t.Error("translation did not change the name, so the name does not track the real bind")
	}
}

// A delegate with no workspace runs in the backend's scratch dir, which is
// derived from the task id already. Fingerprinting there would defeat the
// resume-by-task-id that scratch containers exist for.
func TestContainerNameHasNoFingerprintWithoutAWorkspace(t *testing.T) {
	name, err := ContainerName("task-1", SandboxSpec{}, "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if name != containerNamePrefix+"task-1" {
		t.Errorf("name = %q, want the bare prefixed task id", name)
	}
}

// The control socket is the same channel for every delegate on the host, so it
// distinguishes nothing and is excluded. Including it would churn every name
// the moment the socket moved.
func TestContainerNameIgnoresTheControlSocket(t *testing.T) {
	base := specWithWorkspace("/repo")
	withSocket := SandboxSpec{Mounts: append(append([]SandboxMount{}, base.Mounts...),
		SandboxMount{Source: "/run/a.sock", Target: "/run/b.sock", Kind: SandboxControlSocket})}

	a, _ := ContainerName("task-1", base, "")
	b, _ := ContainerName("task-1", withSocket, "")
	if a != b {
		t.Errorf("the control socket changed the name: %q vs %q", a, b)
	}
}

// Truncating the SUFFIX would collide two different mount sets onto one name,
// which is the exact failure the fingerprint exists to prevent. The body gives
// way instead.
func TestContainerNameKeepsTheFingerprintWhenTruncating(t *testing.T) {
	long := strings.Repeat("a", 400)
	a, err := ContainerName(long, specWithWorkspace("/repo-a"), "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	b, err := ContainerName(long, specWithWorkspace("/repo-b"), "")
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if len(a) > containerNameMax {
		t.Errorf("name is %d bytes, over the %d bound", len(a), containerNameMax)
	}
	if a == b {
		t.Error("two mount sets collided on one name after truncation")
	}
}

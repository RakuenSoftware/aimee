package delegates

import (
	"strings"
	"testing"
)

// The injection boundary. A package name goes into a shell command inside
// `docker build`, and that build runs WITH network access -- so anything that
// escapes the name is arbitrary code execution during image construction.
func TestSandboxDockerfileRefusesShellInPackageNames(t *testing.T) {
	hostile := []string{
		"curl; rm -rf /",
		"curl && wget evil",
		"$(id)",
		"`id`",
		"pkg|tee",
		"pkg>out",
		"pkg out",       // a space is two arguments
		"pkg\nRUN evil", // a newline is a new Dockerfile instruction
		"-flag",         // must start alphanumeric
		"",              // empty
		"pkg'quote",
		"pkg\"quote",
		"pkg\\escape",
	}
	for _, pkg := range hostile {
		if _, err := SandboxDockerfile("ubuntu:22.04", []string{pkg}); err == nil {
			t.Errorf("accepted hostile package name %q", pkg)
		}
	}
}

// Real package names must still work, including the punctuation Debian uses.
func TestSandboxDockerfileAcceptsRealPackageNames(t *testing.T) {
	packages := []string{"gcc", "make", "g++", "libssl-dev", "python3.11", "gcc:amd64", "ca_certs"}
	df, err := SandboxDockerfile("ubuntu:22.04", packages)
	if err != nil {
		t.Fatalf("rejected real package names: %v", err)
	}
	for _, pkg := range packages {
		if !strings.Contains(df, pkg) {
			t.Errorf("%q missing from the dockerfile", pkg)
		}
	}
	if !strings.HasPrefix(df, "FROM ubuntu:22.04\n") {
		t.Errorf("dockerfile does not start with the base: %q", df)
	}
	// The package index must not be left in the image.
	if !strings.Contains(df, "rm -rf /var/lib/apt/lists/*") {
		t.Error("apt lists are not cleaned up in the same layer")
	}
}

// A base reference is the same injection boundary, and legitimately contains
// registry and repository separators.
func TestSandboxDockerfileBaseValidation(t *testing.T) {
	for _, base := range []string{
		"ubuntu:22.04",
		"ghcr.io/org/image:tag",
		"registry.example.com:5000/team/img:1.2.3",
	} {
		if _, err := SandboxDockerfile(base, nil); err != nil {
			t.Errorf("rejected valid base %q: %v", base, err)
		}
	}
	for _, base := range []string{
		"", "ubuntu:22.04 && evil", "$(id)", "ubuntu\nRUN evil", "ubuntu;rm",
	} {
		if _, err := SandboxDockerfile(base, nil); err == nil {
			t.Errorf("accepted hostile base %q", base)
		}
	}
}

// A project that names a base and installs nothing should not get an apt
// invocation it never asked for.
func TestSandboxDockerfileWithNoPackagesIsJustTheBase(t *testing.T) {
	df, err := SandboxDockerfile("ubuntu:22.04", nil)
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if df != "FROM ubuntu:22.04\n" {
		t.Errorf("dockerfile = %q, want only the FROM line", df)
	}
}

// Identical content must resolve to the same tag, or every turn rebuilds an
// image that already exists.
func TestSandboxContentTagIsStableAndContentAddressed(t *testing.T) {
	a, _ := SandboxDockerfile("ubuntu:22.04", []string{"gcc", "make"})
	again, _ := SandboxDockerfile("ubuntu:22.04", []string{"gcc", "make"})
	different, _ := SandboxDockerfile("ubuntu:22.04", []string{"gcc"})

	if SandboxContentTag(a) != SandboxContentTag(again) {
		t.Error("the same content produced two different tags")
	}
	if SandboxContentTag(a) == SandboxContentTag(different) {
		t.Error("different content produced the same tag")
	}
	tag := SandboxContentTag(a)
	if !strings.HasPrefix(tag, "aimee-sbx:") {
		t.Errorf("tag = %q, want the aimee-sbx prefix", tag)
	}
	// The existing tag shape must be preserved or previously built images stop
	// being found and every sandbox rebuilds.
	if got := len(strings.TrimPrefix(tag, "aimee-sbx:")); got != 12 {
		t.Errorf("tag hash length = %d, want 12", got)
	}
}

// The repo's own declaration wins: a project that says it needs a Rust
// toolchain is right about that, whatever the workspace or installation prefers.
func TestResolveSandboxImagePrecedence(t *testing.T) {
	all := SandboxImageCandidates{
		Project: "proj:1", Workspace: "ws:1", Global: "glob:1",
	}
	ref, source, err := ResolveSandboxImage(all)
	if err != nil || ref != "proj:1" || source != SandboxImageProject {
		t.Errorf("ref=%q source=%v err=%v, want the project image", ref, source, err)
	}

	ref, source, _ = ResolveSandboxImage(SandboxImageCandidates{Workspace: "ws:1", Global: "glob:1"})
	if ref != "ws:1" || source != SandboxImageWorkspace {
		t.Errorf("ref=%q source=%v, want the workspace image", ref, source)
	}

	ref, source, _ = ResolveSandboxImage(SandboxImageCandidates{Global: "glob:1"})
	if ref != "glob:1" || source != SandboxImageGlobal {
		t.Errorf("ref=%q source=%v, want the global image", ref, source)
	}

	// Nothing configured is not an error: the caller runs its default image.
	ref, source, err = ResolveSandboxImage(SandboxImageCandidates{})
	if ref != "" || source != SandboxImageNone || err != nil {
		t.Errorf("ref=%q source=%v err=%v, want no image and no error", ref, source, err)
	}
}

// Falling through to a less specific image would run the delegate with a
// toolchain nobody chose, so an invalid reference is refused instead.
func TestResolveSandboxImageRefusesRatherThanFallingThrough(t *testing.T) {
	_, _, err := ResolveSandboxImage(SandboxImageCandidates{
		Project: "evil;rm -rf /", Global: "ubuntu:22.04",
	})
	if err == nil {
		t.Error("an invalid project image silently fell through to the global default")
	}
}

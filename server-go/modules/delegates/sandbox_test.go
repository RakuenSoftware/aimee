package delegates

import (
	"errors"
	"strings"
	"testing"
)

func writeReq() SandboxRequest {
	return SandboxRequest{
		WritesAllowed:      true,
		RepoRoot:           "/srv/repo",
		Worktree:           "/srv/repo/.aimee/worktrees/w1/main",
		GitDir:             "/srv/repo/.git/worktrees/w1",
		IsGitCheckout:      true,
		ParentSocketHost:   "/run/aimee/aimee-http.sock",
		ParentSocketTarget: "/run/aimee/aimee-http.sock",
		EgressProxy:        "http://egress:3128",
	}
}

func readReq() SandboxRequest {
	return SandboxRequest{
		WritesAllowed:      false,
		RepoRoot:           "/srv/repo",
		Worktree:           "/srv/repo/.aimee/worktrees/r1/main",
		GitDir:             "/srv/repo/.git/worktrees/r1",
		IsGitCheckout:      true,
		ParentSocketHost:   "/run/aimee/aimee-http.sock",
		ParentSocketTarget: "/run/aimee/aimee-http.sock",
	}
}

func findMount(spec SandboxSpec, target string) (SandboxMount, bool) {
	for _, m := range spec.Mounts {
		if m.Target == target {
			return m, true
		}
	}
	return SandboxMount{}, false
}

// A write delegate gets only its own worktree. The common Git directory is
// readable for object lookup, but the parent checkout's working files are not
// exposed at all.
func TestSandboxWriteDelegateMountLayering(t *testing.T) {
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if spec.ReadOnly {
		t.Error("a write role produced a read-only sandbox")
	}

	if _, ok := findMount(spec, "/srv/repo"); ok {
		t.Error("parent checkout was exposed to the delegate")
	}
	common, ok := findMount(spec, "/srv/repo/.git")
	if !ok || !common.ReadOnly || common.Kind != SandboxGitMetadata {
		t.Errorf("common git mount = %+v, want present, metadata-only, and read-only", common)
	}
	worktree, ok := findMount(spec, "/srv/repo/.aimee/worktrees/w1/main")
	if !ok || worktree.ReadOnly {
		t.Errorf("worktree mount = %+v, want present and writable", worktree)
	}
	// git status refreshes its index here, so a read-only .git breaks it.
	gitDir, ok := findMount(spec, "/srv/repo/.git/worktrees/w1")
	if !ok || gitDir.ReadOnly {
		t.Errorf("git dir mount = %+v, want present and writable", gitDir)
	}
}

// A read delegate gets its own checkout and the minimum Git metadata it needs;
// it never receives the supervisor's live working files.
func TestSandboxReadOnlyDelegateCannotGetAWritableWorkspace(t *testing.T) {
	spec, err := BuildSandboxSpec(readReq())
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if !spec.ReadOnly {
		t.Fatal("a read role produced a writable sandbox")
	}
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace && !m.ReadOnly {
			t.Errorf("writable workspace mount for a read-only role: %+v", m)
		}
	}
	// Exactly one workspace mount: the delegate's dedicated worktree.
	workspaceMounts := 0
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace {
			workspaceMounts++
		}
	}
	if workspaceMounts != 1 {
		t.Errorf("read-only delegate got %d workspace mounts, want 1", workspaceMounts)
	}
	common, ok := findMount(spec, "/srv/repo/.git")
	if !ok || !common.ReadOnly || common.Kind != SandboxGitMetadata {
		t.Errorf("read-only common git mount = %+v, want read-only metadata", common)
	}
}

// The socket must stay writable or the delegate cannot connect to its parent at
// all -- and it is the ONLY thing exempt from the read-only rule.
func TestSandboxControlSocketStaysWritableForAReadOnlyRole(t *testing.T) {
	spec, err := BuildSandboxSpec(readReq())
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	sock, ok := findMount(spec, "/run/aimee/aimee-http.sock")
	if !ok {
		t.Fatal("no control socket mount")
	}
	if sock.Kind != SandboxControlSocket {
		t.Errorf("socket mount kind = %v, want control socket", sock.Kind)
	}
	if sock.ReadOnly {
		t.Error("control socket mounted read-only; the delegate could not connect")
	}
}

// One flag removes lateral movement and data exfiltration. It is a method, not
// a field, so no caller can set it to anything else.
func TestSandboxHasNoNetwork(t *testing.T) {
	for _, req := range []SandboxRequest{writeReq(), readReq()} {
		spec, err := BuildSandboxSpec(req)
		if err != nil {
			t.Fatalf("build: %v", err)
		}
		if spec.NetworkMode() != "none" {
			t.Errorf("network mode = %q, want none", spec.NetworkMode())
		}
	}
}

// Binding a container runtime socket hands the delegate root-equivalent control
// of the host daemon. Refused by base name, so it is caught at any path.
func TestSandboxRefusesContainerRuntimeSockets(t *testing.T) {
	for _, p := range []string{
		"/var/run/docker.sock",
		"/some/other/place/docker.sock",
		"/run/containerd/containerd.sock",
		"/run/podman/podman.sock",
	} {
		req := writeReq()
		req.ParentSocketHost = p
		if _, err := BuildSandboxSpec(req); err == nil {
			t.Errorf("%s was accepted as a mount source", p)
		}
	}
}

// Without this, any host directory could be mounted into a delegate.
func TestSandboxRequiresAGitCheckout(t *testing.T) {
	req := writeReq()
	req.IsGitCheckout = false
	_, err := BuildSandboxSpec(req)
	if !errors.Is(err, ErrNotGitCheckout) {
		t.Errorf("err = %v, want ErrNotGitCheckout", err)
	}
}

// The container performs no authenticated work, so a credential in its
// environment is a leak with no upside.
func TestSandboxRefusesCredentialsInTheEnvironment(t *testing.T) {
	for _, name := range []string{
		"ANTHROPIC_API_KEY", "GITHUB_TOKEN", "AWS_SECRET_ACCESS_KEY",
		"DB_PASSWORD", "VAULT_CREDENTIAL", "SESSION_ID", "AUTH_HEADER",
	} {
		spec := SandboxSpec{Env: []SandboxEnv{{Name: name, Value: "x"}}}
		if err := ValidateSandboxSpec(spec); err == nil {
			t.Errorf("%s was allowed into the sandbox", name)
		}
	}
	// The proxy variables are not credentials and must survive.
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	var sawProxy bool
	for _, e := range spec.Env {
		if e.Name == "http_proxy" && e.Value == "http://egress:3128" {
			sawProxy = true
		}
		if looksLikeCredential(e.Name) {
			t.Errorf("credential-looking env survived: %s", e.Name)
		}
	}
	if !sawProxy {
		t.Error("http_proxy missing; a no-network delegate could not install software")
	}
}

// Relative paths would resolve against whatever the daemon's cwd happens to be.
func TestSandboxRequiresAbsolutePaths(t *testing.T) {
	req := writeReq()
	req.Worktree = "relative/worktree"
	if _, err := BuildSandboxSpec(req); err == nil {
		t.Error("a relative worktree was accepted")
	}

	req = writeReq()
	req.RepoRoot = "repo"
	if _, err := BuildSandboxSpec(req); err == nil {
		t.Error("a relative repo root was accepted")
	}
}

// ValidateSandboxSpec is the backstop: it must reject a hand-built spec that
// violates an invariant, so the guarantees do not depend on BuildSandboxSpec
// staying correct.
func TestValidateRejectsHandBuiltViolations(t *testing.T) {
	writableWorkspaceForReadRole := SandboxSpec{
		ReadOnly: true,
		Mounts:   []SandboxMount{{Source: "/srv/repo", Target: "/srv/repo"}},
	}
	if err := ValidateSandboxSpec(writableWorkspaceForReadRole); err == nil {
		t.Error("a writable workspace mount for a read-only role was accepted")
	}

	dockerSock := SandboxSpec{
		Mounts: []SandboxMount{{
			Source: "/var/run/docker.sock", Target: "/var/run/docker.sock",
			Kind: SandboxControlSocket,
		}},
	}
	if err := ValidateSandboxSpec(dockerSock); err == nil {
		t.Error("the docker socket was accepted")
	} else if !strings.Contains(err.Error(), "runtime socket") {
		t.Errorf("unexpected error: %v", err)
	}
}

// A PLAIN checkout carries its own .git, so the repo root IS the worktree. It
// gets ONE writable mount.
//
// Two binds on one target is not a layering: the delegate would get whichever
// docker resolved last, so a write role could silently land on the read-only
// copy and only find out when a write failed. The backend has always mounted a
// plain checkout exactly once.
func TestPlainCheckoutGetsOneWritableMount(t *testing.T) {
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed: true, RepoRoot: "/repo", Worktree: "/repo", IsGitCheckout: true,
		ParentSocketHost: "/run/aimee/aimee-http.sock", ParentSocketTarget: ControlSocketTarget,
	})
	if err != nil {
		t.Fatalf("err = %v", err)
	}

	byTarget := map[string]int{}
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace {
			byTarget[m.Target]++
		}
	}
	if byTarget["/repo"] != 1 {
		t.Fatalf("/repo mounted %d times, want exactly 1", byTarget["/repo"])
	}
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace && m.Target == "/repo" && m.ReadOnly {
			t.Error("a write delegate's plain checkout was mounted read-only")
		}
	}
}

// A linked worktree gets common Git metadata without exposing the parent
// checkout's working files.
func TestLinkedWorktreeKeepsParentCheckoutHidden(t *testing.T) {
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed: true, RepoRoot: "/repo", Worktree: "/repo/.aimee/worktrees/d1",
		GitDir: "/repo/.git/worktrees/d1", IsGitCheckout: true,
		ParentSocketHost: "/run/aimee/aimee-http.sock", ParentSocketTarget: ControlSocketTarget,
	})
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	var repoVisible, commonRO, worktreeRW bool
	for _, m := range spec.Mounts {
		if m.Target == "/repo" {
			repoVisible = true
		}
		if m.Target == "/repo/.git" && m.ReadOnly && m.Kind == SandboxGitMetadata {
			commonRO = true
		}
		if m.Target == "/repo/.aimee/worktrees/d1" && !m.ReadOnly {
			worktreeRW = true
		}
	}
	if repoVisible {
		t.Error("the parent checkout's working files are visible")
	}
	if !commonRO {
		t.Error("the common Git metadata is not mounted read-only")
	}
	if !worktreeRW {
		t.Error("the worktree is not writable")
	}
}

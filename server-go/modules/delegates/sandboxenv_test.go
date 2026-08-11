package delegates

import "testing"

func envValue(spec SandboxSpec, name string) (string, bool) {
	for _, e := range spec.Env {
		if e.Name == name {
			return e.Value, true
		}
	}
	return "", false
}

func envSpec(t *testing.T) SandboxSpec {
	t.Helper()
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed:      true,
		RepoRoot:           "/repo",
		Worktree:           "/repo/.aimee/worktrees/d1",
		GitDir:             "/repo/.git/worktrees/d1",
		IsGitCheckout:      true,
		ParentSocketHost:   "/run/aimee/aimee-http.sock",
		ParentSocketTarget: "/run/aimee.sock",
		EgressProxy:        "http://127.0.0.1:3129",
		RunAsUser:          "1000:1000",
	})
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}
	return spec
}

// The container's only outward channel is the parent socket. Without this the
// in-container CLI does not know where it is, and every tool call goes nowhere.
func TestSandboxEnvPointsTheCLIAtTheParentSocket(t *testing.T) {
	got, ok := envValue(envSpec(t), "AIMEE_API_ENDPOINT")
	if !ok {
		t.Fatal("AIMEE_API_ENDPOINT is absent, so the delegate cannot reach its parent")
	}
	if got != "unix:/run/aimee.sock" {
		t.Errorf("AIMEE_API_ENDPOINT = %q, want the in-container socket path", got)
	}
}

// A delegate with no socket has nothing to point at, and a dangling endpoint
// would make every tool call fail slowly rather than not be attempted.
func TestSandboxEnvOmitsTheEndpointWithoutASocket(t *testing.T) {
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed: true, RepoRoot: "/repo", Worktree: "/repo", IsGitCheckout: true,
	})
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}
	if _, ok := envValue(spec, "AIMEE_API_ENDPOINT"); ok {
		t.Error("an endpoint was set with no socket to point at")
	}
}

// Package managers disagree about which spelling they read. One honoured while
// the other is not is a fetch that silently bypasses the proxy entirely.
func TestSandboxEnvSetsBothProxySpellings(t *testing.T) {
	spec := envSpec(t)
	for _, name := range []string{"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"} {
		got, ok := envValue(spec, name)
		if !ok {
			t.Errorf("%s is absent", name)
			continue
		}
		if got != "http://127.0.0.1:3129" {
			t.Errorf("%s = %q", name, got)
		}
	}
	// Loopback stays direct, or the forwarder tries to proxy to itself.
	for _, name := range []string{"no_proxy", "NO_PROXY"} {
		if got, ok := envValue(spec, name); !ok || got != "localhost,127.0.0.1" {
			t.Errorf("%s = %q (present=%v)", name, got, ok)
		}
	}
}

func TestSandboxEnvHasNoProxyWithoutOne(t *testing.T) {
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed: true, RepoRoot: "/repo", Worktree: "/repo", IsGitCheckout: true,
	})
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}
	for _, name := range []string{"http_proxy", "HTTP_PROXY", "no_proxy"} {
		if _, ok := envValue(spec, name); ok {
			t.Errorf("%s was set with no egress proxy configured", name)
		}
	}
}

// A linked worktree resolves its .git to a SEPARATELY mounted gitdir whose
// ownership git checks independently, so `git status` trips "detected dubious
// ownership" and refuses -- breaking the delegate's inspection of its own tree.
func TestSandboxEnvTrustsTheMountedTreesForGit(t *testing.T) {
	spec := envSpec(t)
	want := map[string]string{
		"GIT_CONFIG_COUNT":   "1",
		"GIT_CONFIG_KEY_0":   "safe.directory",
		"GIT_CONFIG_VALUE_0": "*",
	}
	for name, value := range want {
		if got, ok := envValue(spec, name); !ok || got != value {
			t.Errorf("%s = %q (present=%v), want %q", name, got, ok, value)
		}
	}
}

// GIT_CONFIG_KEY_0 contains "KEY" and would otherwise trip the credential scan.
// It must survive validation -- the spec builder validates its own output, so a
// false positive here refuses every delegate.
func TestSandboxGitConfigSurvivesTheCredentialScan(t *testing.T) {
	if err := ValidateSandboxSpec(envSpec(t)); err != nil {
		t.Fatalf("a valid spec was refused: %v", err)
	}
}

// The allowlist must not have widened the scan. A real key still gets refused.
func TestSandboxStillRefusesRealCredentials(t *testing.T) {
	for _, name := range []string{
		"ANTHROPIC_API_KEY", "GITHUB_TOKEN", "AWS_SECRET_ACCESS_KEY",
		"GIT_CONFIG_KEY_1", // not the allowlisted name
	} {
		spec := SandboxSpec{Env: []SandboxEnv{{Name: name, Value: "x"}}}
		if err := ValidateSandboxSpec(spec); err == nil {
			t.Errorf("%s reached the sandbox", name)
		}
	}
}

// Containers run as root by default. Every file the delegate created in the
// caller's checkout would land root-owned: the user could not edit or delete
// their own files afterwards, and git would refuse the tree outright.
func TestDockerArgsRunAsTheTreeOwner(t *testing.T) {
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec: envSpec(t), ContainerName: "c1", Image: "ubuntu:22.04",
	})
	if err != nil {
		t.Fatalf("DockerCreateArgs: %v", err)
	}
	i := -1
	for n, a := range args {
		if a == "--user" {
			i = n
		}
	}
	if i < 0 || i+1 >= len(args) || args[i+1] != "1000:1000" {
		t.Errorf("argv does not run as the tree owner: %v", args)
	}
}

// The image's own user stands when none is given, rather than an empty --user
// docker would reject.
func TestDockerArgsOmitsUserWhenUnset(t *testing.T) {
	spec := envSpec(t)
	spec.User = ""
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec: spec, ContainerName: "c1", Image: "ubuntu:22.04",
	})
	if err != nil {
		t.Fatalf("DockerCreateArgs: %v", err)
	}
	for _, a := range args {
		if a == "--user" {
			t.Errorf("argv carries an empty --user: %v", args)
		}
	}
}

// The control socket's source is resolved by the caller inspecting its OWN
// container, which is better information than the mount table and available for
// that path alone. Translating it again would re-map an already-host path and
// point the delegate's only outward channel at a directory docker would then
// silently create -- an empty dir where the socket should be.
func TestControlSocketIsNotTranslatedTwice(t *testing.T) {
	spec, err := BuildSandboxSpec(SandboxRequest{
		WritesAllowed:      true,
		RepoRoot:           "/repo",
		Worktree:           "/repo",
		IsGitCheckout:      true,
		ParentSocketHost:   "/host/run/aimee-http.sock",
		ParentSocketTarget: "/run/aimee.sock",
	})
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}

	// A table that WOULD rewrite the socket source if it were applied to it.
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec: spec, ContainerName: "c1", Image: "ubuntu:22.04",
		MountTable: "/host\t/elsewhere\n/repo\t/host/checkout",
	})
	if err != nil {
		t.Fatalf("DockerCreateArgs: %v", err)
	}

	var sawSocket, sawWorkspace bool
	for _, a := range args {
		if a == "/host/run/aimee-http.sock:/run/aimee.sock" {
			sawSocket = true
		}
		if a == "/elsewhere/run/aimee-http.sock:/run/aimee.sock" {
			t.Error("the control socket source was translated a second time")
		}
		if a == "/host/checkout:/repo" {
			sawWorkspace = true
		}
	}
	if !sawSocket {
		t.Errorf("the socket bind is missing or altered: %v", args)
	}
	// ...and the workspace IS still translated, so the exemption stays narrow.
	if !sawWorkspace {
		t.Errorf("the workspace source was not translated: %v", args)
	}
}

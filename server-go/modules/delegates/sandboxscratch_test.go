package delegates

import (
	"strings"
	"testing"
)

func scratchRequest() SandboxRequest {
	return SandboxRequest{
		ScratchDir:         "/var/cache/aimee/delegates/d1",
		ScratchTarget:      "/workspace",
		ParentSocketHost:   "/run/aimee/aimee-http.sock",
		ParentSocketTarget: "/run/aimee.sock",
	}
}

// A delegate with no repository still gets a container: one writable directory
// aimee made for it, and nothing else.
func TestScratchContainerHasOneWritableMount(t *testing.T) {
	spec, err := BuildSandboxSpec(scratchRequest())
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}

	var scratch []SandboxMount
	for _, m := range spec.Mounts {
		if m.Kind == SandboxScratch {
			scratch = append(scratch, m)
		}
		if m.Kind == SandboxWorkspace {
			t.Errorf("a scratch container was given a workspace mount: %+v", m)
		}
	}
	if len(scratch) != 1 {
		t.Fatalf("got %d scratch mounts, want 1", len(scratch))
	}
	if scratch[0].ReadOnly {
		t.Error("the scratch dir is read-only, so the delegate has nowhere to work")
	}
	if scratch[0].Target != "/workspace" {
		t.Errorf("scratch target = %q", scratch[0].Target)
	}
}

// The git-checkout rule exists to refuse an ARBITRARY HOST directory. A path
// aimee made under its own cache is not that, so it is exempt -- but only when
// it is genuinely the whole request.
func TestScratchContainerIsExemptFromTheGitCheckoutRule(t *testing.T) {
	req := scratchRequest()
	req.IsGitCheckout = false // never set for a scratch dir
	if _, err := BuildSandboxSpec(req); err != nil {
		t.Fatalf("a scratch container was refused for not being a git checkout: %v", err)
	}
}

// The exemption must not become a way to smuggle a host tree past the rule, so
// a request that is both is refused rather than resolved in favour of one.
func TestScratchAndRepositoryTogetherAreRefused(t *testing.T) {
	cases := map[string]func(*SandboxRequest){
		"worktree": func(r *SandboxRequest) { r.Worktree = "/repo" },
		"repo":     func(r *SandboxRequest) { r.RepoRoot = "/repo" },
		"gitdir":   func(r *SandboxRequest) { r.GitDir = "/repo/.git" },
	}
	for name, mutate := range cases {
		req := scratchRequest()
		mutate(&req)
		if _, err := BuildSandboxSpec(req); err == nil {
			t.Errorf("a scratch request naming a %s was accepted", name)
		}
	}
}

func TestScratchPathsMustBeAbsolute(t *testing.T) {
	for name, mutate := range map[string]func(*SandboxRequest){
		"relative source": func(r *SandboxRequest) { r.ScratchDir = "cache/d1" },
		"relative target": func(r *SandboxRequest) { r.ScratchTarget = "workspace" },
		"empty target":    func(r *SandboxRequest) { r.ScratchTarget = "" },
	} {
		req := scratchRequest()
		mutate(&req)
		if _, err := BuildSandboxSpec(req); err == nil {
			t.Errorf("%s was accepted", name)
		}
	}
}

// Every guarantee still applies. The scratch shape shares the channel and the
// environment with the repository shape rather than reimplementing them.
func TestScratchContainerKeepsEveryGuarantee(t *testing.T) {
	spec, err := BuildSandboxSpec(scratchRequest())
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}
	if spec.NetworkMode() != "none" {
		t.Errorf("network mode = %q", spec.NetworkMode())
	}
	if _, ok := envValue(spec, "AIMEE_API_ENDPOINT"); !ok {
		t.Error("a scratch delegate cannot reach its parent")
	}
	if _, ok := envValue(spec, "GIT_CONFIG_COUNT"); !ok {
		t.Error("the git trust config is missing")
	}
	var socket bool
	for _, m := range spec.Mounts {
		if m.Kind == SandboxControlSocket {
			socket = true
		}
	}
	if !socket {
		t.Error("the control socket is not mounted")
	}
	if err := ValidateSandboxSpec(spec); err != nil {
		t.Errorf("the scratch spec does not validate: %v", err)
	}
}

// The runtime socket is refused here too: the shared validation is not skipped
// on this path.
func TestScratchContainerRefusesTheRuntimeSocket(t *testing.T) {
	req := scratchRequest()
	req.ScratchDir = "/var/run/docker.sock"
	if _, err := BuildSandboxSpec(req); err == nil {
		t.Error("a scratch container was allowed to bind the runtime socket")
	}
}

// The scratch dir reaches the argv as a real bind, writable.
func TestScratchContainerRendersItsMount(t *testing.T) {
	spec, err := BuildSandboxSpec(scratchRequest())
	if err != nil {
		t.Fatalf("BuildSandboxSpec: %v", err)
	}
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec: spec, ContainerName: "c1", Image: "ubuntu:22.04",
	})
	if err != nil {
		t.Fatalf("DockerCreateArgs: %v", err)
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "/var/cache/aimee/delegates/d1:/workspace") {
		t.Errorf("the scratch mount is not in the argv: %v", args)
	}
	if strings.Contains(joined, "/workspace:ro") {
		t.Errorf("the scratch mount was rendered read-only: %v", args)
	}
}

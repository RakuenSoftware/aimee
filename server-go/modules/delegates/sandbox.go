package delegates

import (
	"errors"
	"fmt"
	"path"
	"strings"
)

// What a delegate's container is allowed to be.
//
// A delegate is a completely sandboxed container: files arrive by bind mount,
// it has no network except one socket to its parent, and it holds no
// credentials. This file decides the container's shape and nothing else -- it
// creates nothing, so every safety property below is checkable by a test rather
// than by watching a container run.
//
// The properties that must hold, and which the tests pin:
//
//   - no network, ever
//   - the docker socket is never mounted, at any path
//   - a read-only role can only ever receive read-only mounts
//   - the workspace must be a git checkout
//   - no credential ever appears in the environment

// ErrNotGitCheckout is returned when the workspace is not a git checkout.
// Without this, any host directory could be mounted into a delegate.
var ErrNotGitCheckout = errors.New("workspace is not a git checkout")

// dockerSocketNames are refused as a mount source at ANY path. Binding the
// docker socket hands a delegate root-equivalent control of the host daemon.
var dockerSocketNames = []string{"docker.sock", "containerd.sock", "podman.sock"}

// credentialEnvMarkers name environment variables that must never reach the
// container. The container performs no authenticated work: LLM traffic is
// proxied through the parent, and anything else needing credentials is done by
// a module over the bus.
var credentialEnvMarkers = []string{
	"KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL", "AUTH",
	"SESSION", "COOKIE", "PRIVATE",
}

// SandboxMountKind separates the workspace from the control channel.
//
// The distinction is load-bearing: a read-only delegate must not receive a
// writable WORKSPACE mount, but its parent socket has to stay writable, because
// connecting to a unix socket requires write permission on it. Without the
// distinction the rule is either too weak (allow any writable mount) or too
// strong (a read-only delegate cannot talk to its parent at all).
type SandboxMountKind int

const (
	// SandboxWorkspace is repository content.
	SandboxWorkspace SandboxMountKind = iota
	// SandboxControlSocket is the single outward channel to the parent.
	SandboxControlSocket
	// SandboxScratch is a working directory aimee created for a delegate that
	// has no repository. It is not caller-supplied, which is the whole reason it
	// is a separate kind -- see ScratchDir.
	SandboxScratch
)

// SandboxMount is one bind mount in the container specification.
type SandboxMount struct {
	Source   string
	Target   string
	ReadOnly bool
	Kind     SandboxMountKind
}

// SandboxEnv is one environment entry.
type SandboxEnv struct {
	Name  string
	Value string
}

// SandboxRequest is what the caller knows about the run. Every filesystem fact
// is supplied rather than discovered: this module does not stat the workspace,
// because the workspace owns its files.
type SandboxRequest struct {
	// WritesAllowed is whether THIS delegate may write its worktree.
	//
	// Not derived from the role here, deliberately. The role's default is one
	// input (stage 10), but the caller narrows it further -- a write role whose
	// prompt does not ask for writes does not get a writable tree. Re-deriving
	// from the role would make this module disagree with the decision the caller
	// actually made, and the disagreement would show up as a delegate writing
	// into a tree the caller had already ruled read-only.
	WritesAllowed bool

	// RepoRoot is the parent checkout. Mounted read-only for a write delegate
	// so the tree is readable but only the worktree is writable.
	RepoRoot string
	// Worktree is this delegate's own worktree when the role writes, or the
	// PARENT's worktree when it does not.
	Worktree string
	// GitDir is this delegate's git metadata directory. Only meaningful for a
	// write delegate; `git status` refreshes its index there.
	GitDir string
	// IsGitCheckout is the caller's answer to "is Worktree a git checkout".
	IsGitCheckout bool

	// ParentSocketHost is the host path of the parent module's unix socket --
	// the single outward channel. ParentSocketTarget is where it appears
	// inside the container.
	ParentSocketHost   string
	ParentSocketTarget string

	// EgressProxy, when set, becomes http_proxy so a no-network delegate can
	// still install software through the narrow update whitelist.
	EgressProxy string

	// ScratchDir is a directory AIMEE created for a delegate that has no
	// repository at all, mounted at ScratchTarget.
	//
	// It is exempt from the git-checkout rule, and that exemption is safe only
	// because of what the rule is for: refusing to bind an ARBITRARY HOST
	// directory into a delegate. A path aimee just made under its own cache is
	// categorically not that. To keep the exemption from becoming a way to
	// smuggle one in, a scratch request may not also name a repository -- the
	// two are mutually exclusive and the builder refuses a request with both.
	ScratchDir    string
	ScratchTarget string

	// RunAsUser is "<uid>:<gid>" and must be set whenever a host tree is
	// mounted. Containers run as root by default, so every file the delegate
	// created in the caller's checkout would land root-owned: the user could not
	// edit or delete their own files afterwards, and git would refuse the tree
	// outright with "detected dubious ownership".
	RunAsUser string
}

// SandboxSpec is the container specification. NetworkMode is not a field the
// caller can set: it is always none.
type SandboxSpec struct {
	Mounts   []SandboxMount
	Env      []SandboxEnv
	ReadOnly bool // the role does not write, so nothing is mounted writable
	// User is "<uid>:<gid>", empty to keep the image's own user.
	User string
}

// NetworkMode is always "none". It is a method rather than a field so no caller
// can construct a spec with a network, and no future edit can widen it by
// assignment.
func (SandboxSpec) NetworkMode() string { return "none" }

// isDockerSocket reports whether a path names a container runtime socket,
// judged by base name so it is caught at any location.
func isDockerSocket(p string) bool {
	base := path.Base(strings.TrimRight(p, "/"))
	for _, name := range dockerSocketNames {
		if base == name {
			return true
		}
	}
	return false
}

// sandboxEnvAllowed names variables the container genuinely needs that would
// otherwise trip the marker scan below.
//
// GIT_CONFIG_KEY_0 contains "KEY" and is not a secret: it carries the literal
// string "safe.directory". Naming the exceptions explicitly keeps the scan as
// broad as it should be -- narrowing the markers to let this through would let
// a real key through with it.
var sandboxEnvAllowed = map[string]bool{
	"GIT_CONFIG_KEY_0": true,
}

// looksLikeCredential reports whether an environment name suggests a secret.
// Deliberately broad: a false positive costs a delegate one variable it should
// not have needed, a false negative puts a credential inside the sandbox.
func looksLikeCredential(name string) bool {
	if sandboxEnvAllowed[name] {
		return false
	}
	upper := strings.ToUpper(name)
	for _, marker := range credentialEnvMarkers {
		if strings.Contains(upper, marker) {
			return true
		}
	}
	return false
}

func cleanAbs(p string) (string, bool) {
	if p == "" || !strings.HasPrefix(p, "/") {
		return "", false
	}
	return path.Clean(p), true
}

// buildScratchSpec is the container for a delegate with no repository.
//
// One writable mount and nothing else: there is no repo to layer beneath it and
// no git metadata to expose. Everything the sandbox guarantees still holds --
// the network, the runtime socket and the credential rules are applied by the
// shared tail below, not skipped here.
func buildScratchSpec(req SandboxRequest) (SandboxSpec, error) {
	var spec SandboxSpec

	// The exemption is for aimee's OWN directory. A request that also names a
	// repository is not that, and is refused rather than resolved in favour of
	// one of them.
	if req.Worktree != "" || req.RepoRoot != "" || req.GitDir != "" {
		return spec, fmt.Errorf(
			"a scratch container cannot also mount a repository (worktree=%q repo=%q gitdir=%q)",
			req.Worktree, req.RepoRoot, req.GitDir)
	}

	source, ok := cleanAbs(req.ScratchDir)
	if !ok {
		return spec, fmt.Errorf("scratch dir must be an absolute path, got %q", req.ScratchDir)
	}
	target, ok := cleanAbs(req.ScratchTarget)
	if !ok {
		return spec, fmt.Errorf("scratch target must be an absolute path, got %q", req.ScratchTarget)
	}

	// Writable regardless of the role: the delegate has nowhere else to work,
	// and nothing else can see this directory.
	spec.Mounts = append(spec.Mounts,
		SandboxMount{Source: source, Target: target, Kind: SandboxScratch})

	if err := attachSandboxChannel(&spec, req); err != nil {
		return SandboxSpec{}, err
	}
	if err := ValidateSandboxSpec(spec); err != nil {
		return SandboxSpec{}, err
	}
	return spec, nil
}

// BuildSandboxSpec decides the container's shape for one delegate run.
//
// A write role gets three mounts: the repo read-only so the whole tree is
// readable and a write outside its worktree fails, then its own worktree and
// git directory read-write nested inside. A read-only role gets exactly one
// mount -- the parent's worktree -- and it is read-only because that is the
// supervisor's live branch and the mode is the enforcement, not the request.
func BuildSandboxSpec(req SandboxRequest) (SandboxSpec, error) {
	var spec SandboxSpec

	if req.ScratchDir != "" {
		return buildScratchSpec(req)
	}

	worktree, ok := cleanAbs(req.Worktree)
	if !ok {
		return spec, fmt.Errorf("worktree must be an absolute path, got %q", req.Worktree)
	}
	if !req.IsGitCheckout {
		return spec, fmt.Errorf("%w: %s", ErrNotGitCheckout, worktree)
	}

	write := req.WritesAllowed
	spec.ReadOnly = !write

	if write {
		repo, ok := cleanAbs(req.RepoRoot)
		if !ok {
			return spec, fmt.Errorf("repo root must be an absolute path, got %q", req.RepoRoot)
		}
		// A PLAIN checkout carries its own .git, so the repo root IS the
		// worktree. It needs one writable mount, not a read-only mount of the
		// same path with a writable one layered over it: two binds on one target
		// is not a layering, and the delegate would get whichever docker
		// resolved last -- a coin flip between a writable tree and a read-only
		// one, discovered only when a write failed.
		if repo != worktree {
			// The tree is readable; only the worktree below is writable.
			spec.Mounts = append(spec.Mounts,
				SandboxMount{Source: repo, Target: repo, ReadOnly: true})
		}
		spec.Mounts = append(spec.Mounts,
			SandboxMount{Source: worktree, Target: worktree, ReadOnly: false})
		if gitDir, ok := cleanAbs(req.GitDir); ok {
			// `git status` refreshes its index here, so a read-only .git breaks
			// it. `git commit` still fails, because objects live in the
			// read-only repo -- intended, commits run module-side.
			spec.Mounts = append(spec.Mounts,
				SandboxMount{Source: gitDir, Target: gitDir, ReadOnly: false})
		}
	} else {
		// The parent's worktree. Read-only is enforced by the mount, not by
		// asking the delegate not to write.
		spec.Mounts = append(spec.Mounts,
			SandboxMount{Source: worktree, Target: worktree, ReadOnly: true})
	}

	if err := attachSandboxChannel(&spec, req); err != nil {
		return SandboxSpec{}, err
	}
	if err := ValidateSandboxSpec(spec); err != nil {
		return SandboxSpec{}, err
	}
	return spec, nil
}

// attachSandboxChannel adds everything that is true of EVERY delegate container
// regardless of what it mounts: the one outward channel, the environment that
// makes it usable, and the user it runs as.
//
// Shared by the repository and scratch shapes on purpose. These are the parts
// whose absence is silent -- a container with no AIMEE_API_ENDPOINT starts and
// looks healthy -- so a second copy that fell one variable behind would be
// found by a delegate failing mysteriously, not by a test.
func attachSandboxChannel(spec *SandboxSpec, req SandboxRequest) error {
	if req.ParentSocketHost != "" {
		host, ok := cleanAbs(req.ParentSocketHost)
		if !ok {
			return fmt.Errorf("parent socket must be an absolute path, got %q",
				req.ParentSocketHost)
		}
		target, ok := cleanAbs(req.ParentSocketTarget)
		if !ok {
			return fmt.Errorf("parent socket target must be an absolute path, got %q",
				req.ParentSocketTarget)
		}
		spec.Mounts = append(spec.Mounts,
			SandboxMount{Source: host, Target: target, Kind: SandboxControlSocket})
	}

	if req.ParentSocketTarget != "" {
		// Without this the in-container CLI does not know where its only
		// outward channel is, and every tool call it makes goes nowhere.
		spec.Env = append(spec.Env,
			SandboxEnv{Name: "AIMEE_API_ENDPOINT", Value: "unix:" + req.ParentSocketTarget})
	}

	if req.EgressProxy != "" {
		// Both spellings: package managers disagree about which one they read,
		// and one honoured while the other is not is a fetch that silently
		// bypasses the proxy. no_proxy keeps loopback direct so the forwarder
		// does not try to proxy to itself.
		spec.Env = append(spec.Env,
			SandboxEnv{Name: "http_proxy", Value: req.EgressProxy},
			SandboxEnv{Name: "https_proxy", Value: req.EgressProxy},
			SandboxEnv{Name: "HTTP_PROXY", Value: req.EgressProxy},
			SandboxEnv{Name: "HTTPS_PROXY", Value: req.EgressProxy},
			SandboxEnv{Name: "no_proxy", Value: "localhost,127.0.0.1"},
			SandboxEnv{Name: "NO_PROXY", Value: "localhost,127.0.0.1"})
	}

	// Trust every tree inside the container for git. Even running as the tree's
	// owner, a linked worktree resolves its .git to a SEPARATELY mounted gitdir
	// whose ownership git checks independently, so a plain `git status` still
	// trips "detected dubious ownership" and refuses -- which breaks the
	// delegate's git-based inspection of its own worktree. The container is a
	// single-purpose sandbox holding only the mounted trees, so trusting all of
	// them is safe. These are inherited by every exec, so plain `git` works
	// without the caller adding -c safe.directory.
	spec.Env = append(spec.Env,
		SandboxEnv{Name: "GIT_CONFIG_COUNT", Value: "1"},
		SandboxEnv{Name: "GIT_CONFIG_KEY_0", Value: "safe.directory"},
		SandboxEnv{Name: "GIT_CONFIG_VALUE_0", Value: "*"})

	spec.User = req.RunAsUser
	return nil
}

// ValidateSandboxSpec re-checks a specification against every invariant.
//
// BuildSandboxSpec calls this on its own output, so the guarantees hold even if
// the construction above is later changed carelessly. It is exported so a
// caller can check a spec it did not build.
func ValidateSandboxSpec(spec SandboxSpec) error {
	for _, m := range spec.Mounts {
		if isDockerSocket(m.Source) {
			return fmt.Errorf("refusing to mount a container runtime socket: %s", m.Source)
		}
		if _, ok := cleanAbs(m.Source); !ok {
			return fmt.Errorf("mount source must be an absolute path, got %q", m.Source)
		}
		if _, ok := cleanAbs(m.Target); !ok {
			return fmt.Errorf("mount target must be an absolute path, got %q", m.Target)
		}
		// A read-only role must not receive a writable WORKSPACE mount by any
		// route. The control socket is the sole exemption and must stay
		// writable to be connectable.
		if spec.ReadOnly && m.Kind == SandboxWorkspace && !m.ReadOnly {
			return fmt.Errorf("read-only delegate given a writable workspace mount: %s", m.Target)
		}
	}
	for _, e := range spec.Env {
		if looksLikeCredential(e.Name) {
			return fmt.Errorf("refusing to pass a credential into the sandbox: %s", e.Name)
		}
	}
	return nil
}

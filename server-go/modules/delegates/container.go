package delegates

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

// Bringing a delegate's container up, and refusing to hand it over when its
// isolation cannot be established.
//
// The sequence is create, start, probe, judge. The probe is the point: a
// container is only a sandbox if the runtime actually honoured `--network
// none`, and that is not known until after it starts. So the container is
// brought up, checked, and DESTROYED if the check fails or cannot be completed
// -- a container that failed the check must not be left running, because a
// leftover sandbox with a network is exactly what the check exists to prevent.
//
// Running docker is I/O, so the command runner is injected. The sequencing and
// the failure handling are the part that must be right, and they are testable
// without a daemon.

// CommandRunner runs one command and returns its combined output. An error
// means the command did not complete successfully.
type CommandRunner func(ctx context.Context, name string, args ...string) (string, error)

// ContainerCreationJournal durably hands creation intent and identity back to
// the resource owner before a container may start. It grants no execution policy.
type ContainerCreationJournal interface {
	Intent(DockerCreateRequest) error
	Created(string) error
}

// ContainerRunner brings delegate containers up.
type ContainerRunner struct {
	// Docker is the runtime binary. Empty means "docker".
	Docker string
	// Run executes a command. Required.
	Run CommandRunner
	// MountTable translates bind sources into the daemon's namespace.
	MountTable string
	// RequireImmutableIdentity pins acquisition and posture to the daemon's
	// immutable ID and requires an observed running image/workdir receipt.
	RequireImmutableIdentity bool
	Journal                  ContainerCreationJournal
}

// ContainerResult is what happened.
type ContainerResult struct {
	// Name is the container, whether or not it survived.
	Name string
	// ID is the immutable identity returned by a successful fresh creation.
	// Legacy runners without that receipt retain their ordinary behavior, but
	// cannot establish private verification custody.
	ID      string
	ImageID string
	WorkDir string
	// Refused is set when the container was destroyed rather than handed over.
	Refused bool
	// Reason explains a refusal or a warning, in operator-facing terms.
	Reason string
	// Warned is set when something is worth reporting but the container ran.
	Warned bool
}

func (r ContainerRunner) docker() string {
	if r.Docker == "" {
		return "docker"
	}
	return r.Docker
}

// networkProbeFormat asks the daemon for this container's attached networks and
// their addresses, which is what JudgeIsolation reads.
const networkProbeFormat = `{{json .NetworkSettings.Networks}}`
const mountProbeFormat = `{{range .Mounts}}{{.Source}}|{{.Destination}}|{{.RW}};{{end}}`
const identityProbeFormat = `{{.Id}}`
const receiptProbeFormat = `{{json .}}`
const environmentProbeFormat = `{{range .Config.Env}}{{println .}}{{end}}`

// Start creates and starts a delegate container, then proves it is isolated.
//
// On refusal the container is destroyed before returning. The caller gets a
// result explaining why, not an error to interpret: "we would not run this"
// is an outcome of the run, not a malfunction.
func (r ContainerRunner) Start(ctx context.Context, req DockerCreateRequest) (ContainerResult, error) {
	if req.Spec.ownerHelper && !r.RequireImmutableIdentity {
		return ContainerResult{}, fmt.Errorf("owner helper requires immutable acquisition")
	}
	if r.Run == nil {
		return ContainerResult{}, fmt.Errorf("no command runner configured")
	}
	rendered := req
	rendered.MountTable = r.MountTable
	args, err := DockerCreateArgs(rendered)
	if err != nil {
		return ContainerResult{Name: req.ContainerName}, err
	}

	if r.Journal != nil {
		recorded := req
		recorded.MountTable = r.MountTable
		if err := r.Journal.Intent(recorded); err != nil {
			return ContainerResult{Name: req.ContainerName}, fmt.Errorf("persist creation intent: %w", err)
		}
	}
	created, err := r.Run(ctx, r.docker(), args...)
	if err != nil {
		return ContainerResult{Name: req.ContainerName}, fmt.Errorf("create container: %w", err)
	}
	id, resource := strings.TrimSpace(created), req.ContainerName
	if immutableContainerID(id) {
		resource = id
	} else {
		id = ""
	}
	if r.RequireImmutableIdentity && id == "" {
		return ContainerResult{Name: req.ContainerName, Refused: true, Reason: "immutable creation identity unavailable; creation intent retained"}, nil
	}
	if r.Journal != nil {
		if err := r.Journal.Created(id); err != nil {
			cleanup := r.destroy(ctx, id)
			return ContainerResult{Name: req.ContainerName, ID: id, Refused: true}, fmt.Errorf("persist created identity: %w; cleanup: %v", err, cleanup)
		}
	}
	if _, err := r.Run(ctx, r.docker(), "start", resource); err != nil {
		// Nothing is running, but a created container still exists.
		if cleanupErr := r.destroy(ctx, resource); cleanupErr != nil {
			return ContainerResult{Name: req.ContainerName, Refused: true},
				fmt.Errorf("start container: %w; cleanup: %v", err, cleanupErr)
		}
		return ContainerResult{Name: req.ContainerName}, fmt.Errorf("start container: %w", err)
	}

	return r.verify(ctx, req, resource, id)
}

// Acquire resumes a matching hibernated container when possible, otherwise
// creates a fresh one. Both paths are verified after start before handoff.
func (r ContainerRunner) Acquire(ctx context.Context, req DockerCreateRequest) (ContainerResult, error) {
	if r.Run == nil {
		return ContainerResult{}, fmt.Errorf("no command runner configured")
	}
	if _, err := DockerCreateArgs(req); err != nil {
		return ContainerResult{Name: req.ContainerName}, err
	}
	if req.Spec.ownerHelper {
		return r.Start(ctx, req)
	}
	resource, id := req.ContainerName, ""
	if r.RequireImmutableIdentity {
		observed, err := r.Run(ctx, r.docker(), "inspect", "--format", identityProbeFormat, req.ContainerName)
		if err != nil {
			// A failed name lookup grants no removal authority. Fresh creation is
			// atomic and safely refuses a colliding name without deleting it.
			return r.Start(ctx, req)
		}
		id = strings.TrimSpace(observed)
		if !immutableContainerID(id) {
			return ContainerResult{Name: req.ContainerName, Refused: true, Reason: "immutable resume identity unavailable"}, nil
		}
		resource = id
	}
	if _, err := r.Run(ctx, r.docker(), "start", resource); err != nil {
		if cleanupErr := r.destroy(ctx, resource); cleanupErr != nil {
			return ContainerResult{Name: req.ContainerName, Refused: true}, fmt.Errorf("remove stale container: %w", cleanupErr)
		}
		return r.Start(ctx, req)
	}
	return r.verify(ctx, req, resource, id)
}

type containerProfile struct {
	ID     string `json:"Id"`
	Name   string
	Image  string
	State  struct{ Running bool }
	Config struct {
		WorkingDir  string
		User        string
		Image       string
		Env         *[]string
		Entrypoint  []string
		Cmd         []string
		Healthcheck *ImageHealthcheck
		Labels      map[string]string
	}
	Host struct {
		NetworkMode string
		Privileged  *bool
		PidMode     string
		IpcMode     string
		CapAdd      []string
		CapDrop     []string
		SecurityOpt []string
		Devices     []json.RawMessage
		Binds       []string
	} `json:"HostConfig"`
	Mounts []struct {
		Source      string
		Destination string
		RW          *bool
	}
}

func ownerStartupMatches(req DockerCreateRequest, observed containerProfile) bool {
	return !(observed.Host.Privileged == nil || *observed.Host.Privileged ||
		observed.Host.NetworkMode != "none" || observed.Host.PidMode != "" ||
		(observed.Host.IpcMode != "" && observed.Host.IpcMode != "private") ||
		len(observed.Host.CapAdd) != 0 || len(observed.Host.Devices) != 0 ||
		len(observed.Host.CapDrop) != 1 || !strings.EqualFold(observed.Host.CapDrop[0], "ALL") ||
		!containsNoNewPrivileges(observed.Host.SecurityOpt) ||
		(req.Spec.User != "" && observed.Config.User != req.Spec.User) ||
		len(observed.Config.Entrypoint) != 1 || observed.Config.Entrypoint[0] != OwnerHelperTarget ||
		len(observed.Config.Cmd) != 1 || observed.Config.Cmd[0] != "--idle-guard" ||
		observed.Config.Healthcheck == nil || len(observed.Config.Healthcheck.Test) != 1 || observed.Config.Healthcheck.Test[0] != "NONE")
}

// verify keeps runtime posture and the optional immutable receipt on one resource.
func (r ContainerRunner) verify(ctx context.Context, req DockerCreateRequest, resource, id string) (ContainerResult, error) {
	verdict := VerifyRuntimePosture(req.Spec, r.MountTable, r.probe(ctx, resource))
	result := ContainerResult{Name: req.ContainerName, ID: id}
	if !verdict.Refuse && r.RequireImmutableIdentity {
		raw, err := r.Run(ctx, r.docker(), "inspect", "--format", receiptProbeFormat, id)
		var observed containerProfile
		if err != nil || len(raw) > 1<<20 || json.Unmarshal([]byte(raw), &observed) != nil ||
			observed.ID != id || !observed.State.Running || !strings.HasPrefix(observed.Image, "sha256:") ||
			!immutableContainerID(strings.TrimPrefix(observed.Image, "sha256:")) || observed.Config.WorkingDir != req.WorkDir || req.WorkDir == "" {
			verdict = RuntimePostureVerdict{Refuse: true, Reason: "running container identity unavailable"}
		} else if req.LeaseNonce != "" && !ordinaryStartupMatches(req, observed) {
			verdict = RuntimePostureVerdict{Refuse: true, Reason: "ordinary creator startup identity unavailable"}
		} else if req.Spec.ownerHelper && !ownerStartupMatches(req, observed) {
			verdict = RuntimePostureVerdict{Refuse: true, Reason: "trusted owner startup posture unavailable"}
		} else {
			result.ImageID, result.WorkDir = observed.Image, observed.Config.WorkingDir
		}
	}
	if verdict.Refuse {
		result.Refused, result.Reason = true, verdict.Reason
		if err := r.destroy(ctx, resource); err != nil {
			return result, fmt.Errorf("refused container cleanup: %w", err)
		}
	}
	return result, nil
}

func containsNoNewPrivileges(options []string) bool {
	for _, value := range options {
		if value == "no-new-privileges" || value == "no-new-privileges=true" {
			return true
		}
	}
	return false
}

// probe asks whether the container actually got no network.
func (r ContainerRunner) probe(ctx context.Context, name string) RuntimePostureReport {
	network, networkErr := r.Run(ctx, r.docker(), "inspect", "--format", networkProbeFormat, name)
	mounts, mountsErr := r.Run(ctx, r.docker(), "inspect", "--format", mountProbeFormat, name)
	environment, environmentErr := r.Run(ctx, r.docker(), "inspect", "--format", environmentProbeFormat, name)
	return RuntimePostureReport{Network: network, NetworkFailed: networkErr != nil,
		Mounts: mounts, MountsFailed: mountsErr != nil,
		Environment: environment, EnvironmentFailed: environmentErr != nil}
}

// destroy removes a container, ignoring the result. It runs on paths that are
// already failing, and a removal error must not replace the reason the caller
// needs to see.
func (r ContainerRunner) destroy(ctx context.Context, name string) error {
	// Once strict acquisition has created a resource, its teardown outlives the
	// request. A separate bound prevents cancellation from stranding its exact ID.
	arguments := []string{"rm", "-f"}
	if r.RequireImmutableIdentity {
		if !immutableContainerID(name) {
			return fmt.Errorf("immutable owned identity required for cleanup")
		}
		arguments = append(arguments, "-v")
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(context.WithoutCancel(ctx), 10*time.Second)
		defer cancel()
	}
	arguments = append(arguments, name)
	out, err := r.Run(ctx, r.docker(), arguments...)
	if err != nil && !strings.Contains(strings.ToLower(out), "no such container") {
		return fmt.Errorf("remove container %s: %w", name, err)
	}
	return nil
}

// Stop removes the container once the delegate has finished.
//
// The delegate's work is already on the host -- the worktree is a bind mount --
// so there is nothing to collect first. Strict owned cleanup also removes the
// container's anonymous volumes; named volumes and legacy cleanup are unchanged.
func (r ContainerRunner) Stop(ctx context.Context, name string) error {
	if r.Run == nil {
		return fmt.Errorf("no command runner configured")
	}
	if strings.TrimSpace(name) == "" {
		return fmt.Errorf("container name is required")
	}
	if r.RequireImmutableIdentity {
		return r.destroy(ctx, name)
	}
	if _, err := r.Run(ctx, r.docker(), "rm", "-f", name); err != nil {
		return fmt.Errorf("remove container: %w", err)
	}
	return nil
}

package delegates

import (
	"context"
	"fmt"
	"strings"
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

// ContainerRunner brings delegate containers up.
type ContainerRunner struct {
	// Docker is the runtime binary. Empty means "docker".
	Docker string
	// Run executes a command. Required.
	Run CommandRunner
	// RequireIsolation refuses any container whose isolation cannot be proven,
	// not merely one proven broken.
	RequireIsolation bool
	// MountTable translates bind sources into the daemon's namespace.
	MountTable string
}

// ContainerResult is what happened.
type ContainerResult struct {
	// Name is the container, whether or not it survived.
	Name string
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
const networkProbeFormat = `{{range $k,$v := .NetworkSettings.Networks}}{{$k}}={{$v.IPAddress}};{{end}}`

// Start creates and starts a delegate container, then proves it is isolated.
//
// On refusal the container is destroyed before returning. The caller gets a
// result explaining why, not an error to interpret: "we would not run this"
// is an outcome of the run, not a malfunction.
func (r ContainerRunner) Start(ctx context.Context, req DockerCreateRequest) (ContainerResult, error) {
	if r.Run == nil {
		return ContainerResult{}, fmt.Errorf("no command runner configured")
	}
	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec:          req.Spec,
		ContainerName: req.ContainerName,
		Image:         req.Image,
		WorkDir:       req.WorkDir,
		MountTable:    r.MountTable,
		Command:       req.Command,
	})
	if err != nil {
		return ContainerResult{Name: req.ContainerName}, err
	}

	if _, err := r.Run(ctx, r.docker(), args...); err != nil {
		return ContainerResult{Name: req.ContainerName}, fmt.Errorf("create container: %w", err)
	}
	if _, err := r.Run(ctx, r.docker(), "start", req.ContainerName); err != nil {
		// Nothing is running, but a created container still exists.
		r.destroy(ctx, req.ContainerName)
		return ContainerResult{Name: req.ContainerName}, fmt.Errorf("start container: %w", err)
	}

	verdict := JudgeIsolation(r.probe(ctx, req.ContainerName), r.RequireIsolation)
	if verdict.Refuse {
		// A container that failed the isolation check must not be left running.
		r.destroy(ctx, req.ContainerName)
		return ContainerResult{
			Name: req.ContainerName, Refused: true, Reason: verdict.Reason,
		}, nil
	}
	return ContainerResult{
		Name: req.ContainerName, Warned: verdict.Warn, Reason: verdict.Reason,
	}, nil
}

// probe asks whether the container actually got no network.
func (r ContainerRunner) probe(ctx context.Context, name string) IsolationProbe {
	out, err := r.Run(ctx, r.docker(), "inspect", "--format", networkProbeFormat, name)
	return ParseIsolationProbe(out, err != nil)
}

// destroy removes a container, ignoring the result. It runs on paths that are
// already failing, and a removal error must not replace the reason the caller
// needs to see.
func (r ContainerRunner) destroy(ctx context.Context, name string) {
	_, _ = r.Run(ctx, r.docker(), "rm", "-f", name)
}

// Stop removes the container once the delegate has finished.
//
// The delegate's work is already on the host -- the worktree is a bind mount --
// so there is nothing to collect first. Removing the container discards only
// the container.
func (r ContainerRunner) Stop(ctx context.Context, name string) error {
	if r.Run == nil {
		return fmt.Errorf("no command runner configured")
	}
	if strings.TrimSpace(name) == "" {
		return fmt.Errorf("container name is required")
	}
	if _, err := r.Run(ctx, r.docker(), "rm", "-f", name); err != nil {
		return fmt.Errorf("remove container: %w", err)
	}
	return nil
}

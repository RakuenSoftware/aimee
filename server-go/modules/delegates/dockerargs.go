package delegates

import (
	"fmt"
	"strings"
)

// Turning a sandbox specification into the command that creates the container.
//
// This is the last step where the design's guarantees are still checkable: past
// here they are argv, and a missing flag is a delegate with a network. So the
// argv is built in one place, from the spec, and the tests assert the flags
// rather than trusting that the spec was honoured.

// TranslateMountPath maps a path in THIS process's filesystem namespace to the
// one the container runtime's daemon sees.
//
// Docker resolves bind SOURCES in the daemon's namespace, not the caller's.
// When aimee itself runs in a container the two differ, and passing an
// unstranslated path makes a real directory look absent -- at which point
// docker silently CREATES an empty directory at that source and the delegate
// gets an empty mount instead of the workspace. Silent, and it looks like the
// delegate simply found nothing.
//
// mountTable is "<destination>\t<source>" per line, as the runtime reports this
// container's own mounts. The longest matching destination wins, matched on
// whole path components so /data does not match /database. With no match the
// path is returned unchanged, which is correct for a host-native process that
// has no self container to inspect.
func TranslateMountPath(containerPath, mountTable string) string {
	if containerPath == "" || !strings.HasPrefix(containerPath, "/") {
		return containerPath
	}

	bestLen := 0
	bestSource := ""
	for _, line := range strings.Split(mountTable, "\n") {
		line = strings.TrimRight(line, "\r")
		destination, source, found := strings.Cut(line, "\t")
		if !found || destination == "" || source == "" {
			continue
		}
		if !strings.HasPrefix(destination, "/") || !strings.HasPrefix(source, "/") {
			continue
		}
		if len(destination) <= bestLen || !strings.HasPrefix(containerPath, destination) {
			continue
		}
		// Whole components only: /data must not match /database.
		rest := containerPath[len(destination):]
		if destination != "/" && rest != "" && !strings.HasPrefix(rest, "/") {
			continue
		}
		bestLen = len(destination)
		bestSource = source
	}

	if bestSource == "" {
		return containerPath
	}
	if bestLen == 1 { // the mount is "/", so the whole path is the suffix
		return bestSource + containerPath
	}
	return bestSource + containerPath[bestLen:]
}

// renderBind is one mount in docker's "-v" form, with its source translated
// into the daemon's namespace. The container name fingerprints these same
// strings, so both go through here — a name computed from one rendering while
// another is created is the bug the fingerprint exists to prevent.
// The CONTROL SOCKET is exempt from translation. Its source arrives already in
// the daemon's namespace, because the caller resolves it by inspecting its own
// container -- strictly better information than a mount table, and available
// for that one path only. Translating an already-host path a second time would
// re-map it against the table and point the delegate's only outward channel at
// a directory that does not exist, which docker then silently CREATES. The
// delegate comes up with an empty directory where its socket should be, and
// every tool call it makes fails against it.
func renderBind(m SandboxMount, mountTable string) string {
	source := m.Source
	if mountTable != "" && m.Kind != SandboxControlSocket {
		source = TranslateMountPath(source, mountTable)
	}
	bind := source + ":" + m.Target
	if m.ReadOnly {
		bind += ":ro"
	}
	return bind
}

// DockerCreateRequest is everything needed to phrase the create command.
type DockerCreateRequest struct {
	Spec          SandboxSpec
	ContainerName string
	Image         string
	// WorkDir is the container's working directory, normally the worktree.
	WorkDir string
	// MountTable, when set, translates bind sources into the daemon's
	// namespace. Empty means this process and the daemon share a view.
	MountTable string
	// Command is what the container runs. Empty leaves the image's own.
	Command []string
}

// DockerCreateArgs renders the create command for a sandbox.
//
// It re-validates the spec first. By this point the spec may have travelled,
// and the cost of a spec that lost its guarantees on the way is a delegate with
// a network or a writable copy of the supervisor's branch.
func DockerCreateArgs(req DockerCreateRequest) ([]string, error) {
	if err := ValidateSandboxSpec(req.Spec); err != nil {
		return nil, err
	}
	if req.ContainerName == "" {
		return nil, fmt.Errorf("container name is required")
	}
	if !baseImageValid(req.Image) {
		return nil, fmt.Errorf("invalid image reference: %q", req.Image)
	}

	args := []string{"create", "--name", req.ContainerName}

	// The isolation primitive. Never conditional, never configurable.
	args = append(args, "--network", req.Spec.NetworkMode())

	if req.Spec.User != "" {
		args = append(args, "--user", req.Spec.User)
	}

	for _, m := range req.Spec.Mounts {
		args = append(args, "-v", renderBind(m, req.MountTable))
	}

	for _, e := range req.Spec.Env {
		args = append(args, "-e", e.Name+"="+e.Value)
	}

	if req.WorkDir != "" {
		args = append(args, "-w", req.WorkDir)
	}

	args = append(args, req.Image)
	args = append(args, req.Command...)
	return args, nil
}

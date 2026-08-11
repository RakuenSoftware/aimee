package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Asking the module for the command that creates a delegate's container.
//
// This is the second of the two calls that run a delegate. The caller has the
// plan from stage 11 and the worktree the workspace cut for it; here it gets the
// argv. Everything the sandbox guarantees -- no network, no runtime socket, a
// read-only role that cannot receive a writable workspace mount, no credential
// in the environment -- is decided on this side of the wire and validated again
// before the argv is rendered.
//
// The caller executes the argv. It does not assemble one, which is the point:
// past the argv the guarantees are just flags, and a missing flag is a delegate
// with a network.

const (
	StageLaunchArgs uint32 = 12
	EventLaunchArgs uint32 = 6668

	launchArgsRequestMagic  uint32 = 0x514c4144 /* "DALQ" */
	launchArgsResponseMagic uint32 = 0x534c4144 /* "DALS" */
	launchArgsReqHeaderLen         = 16

	// A mount table is the runtime's report of this container's own mounts, so
	// it is the only field that can be large.
	launchArgsMountTableMax = 1 << 20
	launchArgsStringMax     = 4096
	launchArgsMaxCommand    = 256
)

// launchArgsRequest is the wire form, kept as one struct so the decode below
// reads in the same order the encoder writes.
type launchArgsRequest struct {
	Role          string
	RepoRoot      string
	Worktree      string
	GitDir        string
	IsGitCheckout bool

	ParentSocketHost   string
	ParentSocketTarget string
	EgressProxy        string

	ContainerName string
	Image         string
	WorkDir       string
	MountTable    string
	Command       []string
}

// decodeLaunchArgsRequest reads the request, or reports that it is malformed.
//
// Every string is length-prefixed and bounded. The bound matters: these become
// argv, and an unbounded field is a way to make the caller allocate for a
// command it was never going to run.
func decodeLaunchArgsRequest(request []byte) (launchArgsRequest, bool) {
	var req launchArgsRequest
	if len(request) < launchArgsReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != launchArgsRequestMagic ||
		request[4] != wireVersion || request[5] > 1 {
		return req, false
	}
	req.IsGitCheckout = request[5] == 1
	commandCount := int(binary.LittleEndian.Uint32(request[8:12]))
	if commandCount > launchArgsMaxCommand {
		return req, false
	}

	c := &economicsCursor{buf: request, at: launchArgsReqHeaderLen}
	readString := func(max int) string {
		n := c.u32()
		if n > max {
			c.bad = true
			return ""
		}
		return c.str(n)
	}

	req.Role = readString(roleMax)
	req.RepoRoot = readString(launchArgsStringMax)
	req.Worktree = readString(launchArgsStringMax)
	req.GitDir = readString(launchArgsStringMax)
	req.ParentSocketHost = readString(launchArgsStringMax)
	req.ParentSocketTarget = readString(launchArgsStringMax)
	req.EgressProxy = readString(launchArgsStringMax)
	req.ContainerName = readString(launchArgsStringMax)
	req.Image = readString(launchArgsStringMax)
	req.WorkDir = readString(launchArgsStringMax)
	req.MountTable = readString(launchArgsMountTableMax)

	req.Command = make([]string, 0, commandCount)
	for i := 0; i < commandCount; i++ {
		req.Command = append(req.Command, readString(launchArgsStringMax))
	}

	if c.bad || c.at != len(request) {
		return launchArgsRequest{}, false
	}
	return req, true
}

// handleLaunchArgs renders the create command for one delegate.
//
// The isolation of the plan is derived from the ROLE rather than carried in the
// request. It is the same decision stage 11 made, and re-deriving it means the
// two cannot disagree -- a caller that sent a stale "isolated" flag would
// otherwise get a git directory mounted into a read-only delegate.
func handleLaunchArgs(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	req, ok := decodeLaunchArgsRequest(request)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	plan := WorktreePlan{Isolated: RoleIsWrite(req.Role), ReadOnlyMount: !RoleIsWrite(req.Role)}
	sandboxReq := SandboxRequestFor(plan, req.Role, req.RepoRoot, req.Worktree, req.GitDir,
		req.IsGitCheckout, req.ParentSocketHost, req.ParentSocketTarget, req.EgressProxy)

	spec, err := BuildSandboxSpec(sandboxReq)
	if err != nil {
		// A spec this module refuses to build is not a request to answer
		// partially. The caller gets nothing to run.
		return nil, bus.ModuleStatusInvalidRequest
	}

	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec:          spec,
		ContainerName: req.ContainerName,
		Image:         req.Image,
		WorkDir:       req.WorkDir,
		MountTable:    req.MountTable,
		Command:       req.Command,
	})
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	total := 8
	for _, a := range args {
		total += 4 + len(a)
	}
	response := make([]byte, 8, total)
	binary.LittleEndian.PutUint32(response[0:4], launchArgsResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(args)))
	for _, a := range args {
		var n [4]byte
		binary.LittleEndian.PutUint32(n[:], uint32(len(a)))
		response = append(response, n[:]...)
		response = append(response, a...)
	}
	return response, bus.ModuleStatusOK
}

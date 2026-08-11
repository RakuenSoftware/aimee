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
	// WritesAllowed is the caller's composed answer, not the role's default.
	WritesAllowed bool
	RepoRoot      string
	Worktree      string
	GitDir        string
	IsGitCheckout bool

	ParentSocketHost   string
	ParentSocketTarget string
	EgressProxy        string

	TaskID     string
	Image      string
	WorkDir    string
	MountTable string
	// RunAsUser is "<uid>:<gid>". The caller supplies it because the uid that
	// owns the tree is a fact about the host, not about the delegate.
	RunAsUser string
	// ScratchDir/ScratchTarget describe a delegate with no repository at all.
	ScratchDir    string
	ScratchTarget string
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
		request[4] != wireVersion || request[5] > 3 {
		return req, false
	}
	req.IsGitCheckout = request[5]&1 != 0
	req.WritesAllowed = request[5]&2 != 0
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

	req.RepoRoot = readString(launchArgsStringMax)
	req.Worktree = readString(launchArgsStringMax)
	req.GitDir = readString(launchArgsStringMax)
	req.ParentSocketHost = readString(launchArgsStringMax)
	req.ParentSocketTarget = readString(launchArgsStringMax)
	req.EgressProxy = readString(launchArgsStringMax)
	req.TaskID = readString(launchArgsStringMax)
	req.Image = readString(launchArgsStringMax)
	req.WorkDir = readString(launchArgsStringMax)
	req.MountTable = readString(launchArgsMountTableMax)
	req.RunAsUser = readString(launchArgsStringMax)
	req.ScratchDir = readString(launchArgsStringMax)
	req.ScratchTarget = readString(launchArgsStringMax)

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
// Whether the delegate writes is CARRIED, not re-derived from the role. The
// role's default is only one input: the caller narrows it with a prompt rule
// this module cannot see, so a module that re-derived from the role would
// disagree with the decision the caller actually made -- and would hand a
// writable tree, and a git directory, to a delegate already ruled read-only.
//
// The caller must therefore send the SAME flag it sent to stage 11. It is the
// one fact that has to agree across the two calls, which is why it is a single
// composed boolean rather than a set of inputs each side re-combines.
func handleLaunchArgs(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	req, ok := decodeLaunchArgsRequest(request)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	plan := WorktreePlan{Isolated: req.WritesAllowed, ReadOnlyMount: !req.WritesAllowed}
	sandboxReq := SandboxRequestFor(plan, req.RepoRoot, req.Worktree, req.GitDir,
		req.IsGitCheckout, req.ParentSocketHost, req.ParentSocketTarget, req.EgressProxy)
	sandboxReq.RunAsUser = req.RunAsUser
	sandboxReq.ScratchDir = req.ScratchDir
	sandboxReq.ScratchTarget = req.ScratchTarget

	spec, err := BuildSandboxSpec(sandboxReq)
	if err != nil {
		// A spec this module refuses to build is not a request to answer
		// partially. The caller gets nothing to run.
		return nil, bus.ModuleStatusInvalidRequest
	}

	// The name is computed HERE, from the spec that is about to be rendered, so
	// it cannot describe a different set of mounts than the ones created.
	name, err := ContainerName(req.TaskID, spec, req.MountTable)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	args, err := DockerCreateArgs(DockerCreateRequest{
		Spec:          spec,
		ContainerName: name,
		Image:         req.Image,
		WorkDir:       req.WorkDir,
		MountTable:    req.MountTable,
		Command:       req.Command,
	})
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	total := 12 + len(name)
	for _, a := range args {
		total += 4 + len(a)
	}
	// The container name travels back with the argv. The caller needs it to
	// start, exec into and remove the container, and re-deriving it there would
	// be a second copy of a rule whose entire job is not to have one.
	response := make([]byte, 8, total)
	binary.LittleEndian.PutUint32(response[0:4], launchArgsResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(name)))
	response = append(response, name...)
	var count [4]byte
	binary.LittleEndian.PutUint32(count[:], uint32(len(args)))
	response = append(response, count[:]...)
	for _, a := range args {
		var n [4]byte
		binary.LittleEndian.PutUint32(n[:], uint32(len(a)))
		response = append(response, n[:]...)
		response = append(response, a...)
	}
	return response, bus.ModuleStatusOK
}

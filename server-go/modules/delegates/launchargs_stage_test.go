package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func encodeLaunchArgs(req launchArgsRequest) []byte {
	out := make([]byte, launchArgsReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], launchArgsRequestMagic)
	out[4] = wireVersion
	if req.IsGitCheckout {
		out[5] |= 1
	}
	if req.WritesAllowed {
		out[5] |= 2
	}
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(req.Command)))

	put := func(s string) {
		var n [4]byte
		binary.LittleEndian.PutUint32(n[:], uint32(len(s)))
		out = append(out, n[:]...)
		out = append(out, s...)
	}
	put(req.RepoRoot)
	put(req.Worktree)
	put(req.GitDir)
	put(req.ParentSocketHost)
	put(req.ParentSocketTarget)
	put(req.EgressProxy)
	put(req.TaskID)
	put(req.Image)
	put(req.WorkDir)
	put(req.MountTable)
	put(req.RunAsUser)
	put(req.ScratchDir)
	put(req.ScratchTarget)
	for _, a := range req.Command {
		put(a)
	}
	return out
}

func decodeLaunchArgs(t *testing.T, response []byte) (string, []string) {
	t.Helper()
	if len(response) < 8 {
		t.Fatalf("response is %d bytes", len(response))
	}
	if binary.LittleEndian.Uint32(response[0:4]) != launchArgsResponseMagic {
		t.Fatal("wrong response magic")
	}
	nameLen := int(binary.LittleEndian.Uint32(response[4:8]))
	name := string(response[8 : 8+nameLen])
	at := 8 + nameLen
	count := int(binary.LittleEndian.Uint32(response[at : at+4]))
	at += 4
	args := make([]string, 0, count)
	for i := 0; i < count; i++ {
		n := int(binary.LittleEndian.Uint32(response[at : at+4]))
		at += 4
		args = append(args, string(response[at:at+n]))
		at += n
	}
	if at != len(response) {
		t.Fatalf("decoded %d of %d bytes", at, len(response))
	}
	return name, args
}

func callLaunchArgs(t *testing.T, req launchArgsRequest) ([]string, bus.ModuleStatus) {
	t.Helper()
	_, args, status := callLaunchArgsNamed(t, req)
	return args, status
}

func callLaunchArgsNamed(t *testing.T, req launchArgsRequest) (string, []string, bus.ModuleStatus) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageLaunchArgs}, encodeLaunchArgs(req))
	if status != bus.ModuleStatusOK {
		return "", nil, status
	}
	name, args := decodeLaunchArgs(t, response)
	return name, args, status
}

func writeRequest() launchArgsRequest {
	return launchArgsRequest{
		WritesAllowed:      true,
		RepoRoot:           "/repo",
		Worktree:           "/repo/.aimee/worktrees/d1",
		GitDir:             "/repo/.git/worktrees/d1",
		IsGitCheckout:      true,
		ParentSocketHost:   "/run/aimee/aimee.sock",
		ParentSocketTarget: "/run/aimee.sock",
		TaskID:             "d1",
		Image:              "ubuntu:22.04",
		WorkDir:            "/repo/.aimee/worktrees/d1",
	}
}

func argIndex(args []string, want string) int {
	for i, a := range args {
		if a == want {
			return i
		}
	}
	return -1
}

// The isolation primitive. If this is ever absent the delegate has a network,
// so it is asserted on the rendered argv rather than on the spec.
func TestLaunchArgsAlwaysHasNoNetwork(t *testing.T) {
	for _, writes := range []bool{true, false} {
		req := writeRequest()
		req.WritesAllowed = writes
		role := "a writing delegate"
		if !writes {
			role = "a read-only delegate"
			req.GitDir = ""
			req.RepoRoot = ""
		}
		args, status := callLaunchArgs(t, req)
		if status != bus.ModuleStatusOK {
			t.Fatalf("role %q: status = %v", role, status)
		}
		i := argIndex(args, "--network")
		if i < 0 || i+1 >= len(args) || args[i+1] != "none" {
			t.Errorf("role %q: argv does not carry --network none: %v", role, args)
		}
	}
}

// A write role gets the repo read-only with its own worktree and git dir
// writable nested inside; `git status` refreshes its index in that git dir.
func TestLaunchArgsWriteRoleMountLayering(t *testing.T) {
	args, status := callLaunchArgs(t, writeRequest())
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	joined := strings.Join(args, " ")
	for _, want := range []string{
		"/repo:/repo:ro",
		"/repo/.aimee/worktrees/d1:/repo/.aimee/worktrees/d1",
		"/repo/.git/worktrees/d1:/repo/.git/worktrees/d1",
	} {
		if !strings.Contains(joined, want) {
			t.Errorf("argv missing mount %q: %v", want, args)
		}
	}
	// The worktree and git dir must NOT be read-only.
	if strings.Contains(joined, "/repo/.aimee/worktrees/d1:ro") {
		t.Error("a write delegate's worktree was mounted read-only")
	}
}

// A read-only role mounts the parent's worktree, and the mode is the
// enforcement -- not a request the delegate is asked to honour.
func TestLaunchArgsReadOnlyRoleGetsReadOnlyMount(t *testing.T) {
	req := writeRequest()
	req.WritesAllowed = false
	req.RepoRoot = ""
	req.GitDir = "/repo/.git/worktrees/d1" // supplied, and must be ignored
	req.Worktree = "/repo"

	args, status := callLaunchArgs(t, req)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "/repo:/repo:ro") {
		t.Errorf("the parent worktree was not mounted read-only: %v", args)
	}
	// A git dir belongs to an isolated delegate only. Carrying one here would
	// hand a read-only delegate a writable mount.
	if strings.Contains(joined, ".git/worktrees") {
		t.Errorf("a read-only delegate was given a git dir mount: %v", args)
	}
}

// The module refuses rather than answering partially: a caller that gets no
// argv cannot accidentally run a half-specified container.
func TestLaunchArgsRefusesUnsafeRequests(t *testing.T) {
	cases := map[string]func(*launchArgsRequest){
		"not a git checkout":   func(r *launchArgsRequest) { r.IsGitCheckout = false },
		"relative worktree":    func(r *launchArgsRequest) { r.Worktree = "relative/path" },
		"empty worktree":       func(r *launchArgsRequest) { r.Worktree = "" },
		"no task id":           func(r *launchArgsRequest) { r.TaskID = "" },
		"bad image reference":  func(r *launchArgsRequest) { r.Image = "ubuntu:22.04; rm -rf /" },
		"relative socket host": func(r *launchArgsRequest) { r.ParentSocketHost = "sock" },
	}
	for name, mutate := range cases {
		req := writeRequest()
		mutate(&req)
		if _, status := callLaunchArgs(t, req); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

// Binding the runtime's own socket hands the delegate root-equivalent control
// of the host daemon, so it is refused at any path.
func TestLaunchArgsRefusesTheRuntimeSocket(t *testing.T) {
	for _, sock := range []string{
		"/var/run/docker.sock", "/somewhere/else/docker.sock", "/run/podman.sock",
	} {
		req := writeRequest()
		req.ParentSocketHost = sock
		if _, status := callLaunchArgs(t, req); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", sock, status)
		}
	}
}

// Docker resolves bind SOURCES in the daemon's namespace. An untranslated path
// makes docker silently create an EMPTY directory there, and the delegate gets
// an empty mount instead of the workspace.
func TestLaunchArgsTranslatesMountSources(t *testing.T) {
	req := writeRequest()
	req.MountTable = "/repo\t/host/checkout"

	args, status := callLaunchArgs(t, req)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "/host/checkout:/repo:ro") {
		t.Errorf("the repo source was not translated: %v", args)
	}
	if !strings.Contains(joined, "/host/checkout/.aimee/worktrees/d1:/repo/.aimee/worktrees/d1") {
		t.Errorf("the worktree source was not translated: %v", args)
	}
}

// The egress proxy is how a no-network delegate still installs software through
// the narrow update whitelist.
func TestLaunchArgsCarriesTheEgressProxy(t *testing.T) {
	req := writeRequest()
	req.EgressProxy = "http://127.0.0.1:3129"

	args, status := callLaunchArgs(t, req)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	joined := strings.Join(args, " ")
	for _, want := range []string{"http_proxy=http://127.0.0.1:3129", "https_proxy=http://127.0.0.1:3129"} {
		if !strings.Contains(joined, want) {
			t.Errorf("argv missing %q: %v", want, args)
		}
	}
}

func TestLaunchArgsCarriesTheCommand(t *testing.T) {
	req := writeRequest()
	req.Command = []string{"sleep", "infinity"}

	args, status := callLaunchArgs(t, req)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(args) < 2 || args[len(args)-2] != "sleep" || args[len(args)-1] != "infinity" {
		t.Errorf("the command is not last in argv: %v", args)
	}
	// The image must come immediately before it.
	if args[len(args)-3] != "ubuntu:22.04" {
		t.Errorf("the image does not precede the command: %v", args)
	}
}

func TestLaunchArgsRejectsMalformedRequests(t *testing.T) {
	good := encodeLaunchArgs(writeRequest())

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:8],
		"trailing byte": append(append([]byte{}, good...), 0),
		"truncated":     good[:len(good)-1],
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	tooManyCommands := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(tooManyCommands[8:12], launchArgsMaxCommand+1)
	cases["command count over the bound"] = tooManyCommands

	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[launchArgsReqHeaderLen:launchArgsReqHeaderLen+4], 1<<20)
	cases["role length overruns"] = overrun

	for name, request := range cases {
		_, status := Handle(bus.ModuleInvocation{StageID: StageLaunchArgs}, request)
		if status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestLaunchArgsHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageLaunchArgs, DeadlineNS: 1}
	if _, status := Handle(invocation, encodeLaunchArgs(writeRequest())); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}

package git

// Verification snapshots are private resources, addressed by an opaque lease.
// They never import a model's working-tree files or ambient Git credentials.
import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const verificationSnapshotsMax = 8
const verificationSnapshotTTL = 5 * time.Minute

type verificationSnapshotRequest struct {
	Op          string `json:"op"`
	ProjectRoot string `json:"project_root,omitempty"`
	Device      string `json:"device,omitempty"`
	Inode       string `json:"inode,omitempty"`
	Commit      string `json:"commit,omitempty"`
	Tree        string `json:"tree,omitempty"`
	Lease       string `json:"lease,omitempty"`
}

type verificationSnapshotResponse struct {
	OK     bool   `json:"ok"`
	Lease  string `json:"lease,omitempty"`
	Path   string `json:"path,omitempty"`
	Tree   string `json:"tree,omitempty"`
	Commit string `json:"commit,omitempty"`
	Clean  bool   `json:"clean"`
}

type verificationSnapshot struct {
	root    *os.File
	path    string
	tree    string
	commit  string
	expires time.Time
	timer   *time.Timer
}

var verificationSnapshots = struct {
	sync.Mutex
	creating int
	leases   map[string]*verificationSnapshot
}{leases: map[string]*verificationSnapshot{}}

func snapshotHex(value string, lengths ...int) bool {
	valid := false
	for _, length := range lengths {
		valid = valid || len(value) == length
	}
	if !valid {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// Git can spawn upload-pack. Cancellation must join the whole process group,
// including a child retaining an inherited pipe after its parent exits.
func snapshotGit(ctx context.Context, directory string, capture bool, args ...string) (string, bool) {
	command := exec.CommandContext(ctx, "git", append([]string{"-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false", "-c", "protocol.file.allow=always", "-c", "protocol.ext.allow=never"}, args...)...)
	command.Dir = directory
	command.Env = []string{
		"PATH=" + os.Getenv("PATH"), "HOME=" + directory, "TMPDIR=" + directory,
		"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_TERMINAL_PROMPT=0", "GIT_ALLOW_PROTOCOL=file", "GIT_LFS_SKIP_SMUDGE=1",
		"GIT_CONFIG_COUNT=3", "GIT_CONFIG_KEY_0=core.hooksPath", "GIT_CONFIG_VALUE_0=/dev/null",
		"GIT_CONFIG_KEY_1=uploadpack.packObjectsHook", "GIT_CONFIG_VALUE_1=",
		"GIT_CONFIG_KEY_2=protocol.ext.allow", "GIT_CONFIG_VALUE_2=never",
	}
	if capture {
		command.Env = append(command.Env, "GIT_WORK_TREE="+directory)
	}
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	command.Cancel = func() error {
		if command.Process == nil {
			return os.ErrProcessDone
		}
		return syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
	}
	command.WaitDelay = time.Second
	command.Stderr = io.Discard
	if !capture {
		command.Stdout = io.Discard
		return "", command.Run() == nil
	}
	// Captured commands below return one object ID or a bounded dirty marker.
	var output strings.Builder
	command.Stdout = &snapshotOutput{builder: &output, remaining: 8192}
	err := command.Run()
	return strings.TrimSpace(output.String()), err == nil
}

type snapshotOutput struct {
	builder   *strings.Builder
	remaining int
}

func (output *snapshotOutput) Write(data []byte) (int, error) {
	if len(data) > output.remaining {
		return 0, fmt.Errorf("verification snapshot response exceeded its bound")
	}
	output.remaining -= len(data)
	return output.builder.Write(data)
}

func snapshotRootIdentity(root *os.File, device, inode string) bool {
	info, err := root.Stat()
	if err != nil || !info.IsDir() {
		return false
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && strconv.FormatUint(uint64(stat.Dev), 10) == device &&
		strconv.FormatUint(uint64(stat.Ino), 10) == inode
}

// measureVerificationSource reports the current original source only after
// reopening the exact held identity. It grants no snapshot lease or authority.
func measureVerificationSource(ctx context.Context, request verificationSnapshotRequest) verificationSnapshotResponse {
	empty := verificationSnapshotResponse{}
	if !filepath.IsAbs(request.ProjectRoot) || len(request.ProjectRoot) > 4096 || request.Lease != "" ||
		!snapshotHex(request.Commit, 40, 64) || !snapshotHex(request.Tree, 40, 64) {
		return empty
	}
	source, err := os.OpenFile(request.ProjectRoot, os.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return empty
	}
	defer source.Close()
	if !snapshotRootIdentity(source, request.Device, request.Inode) {
		return empty
	}
	held := fmt.Sprintf("/proc/%d/fd/%d", os.Getpid(), source.Fd())
	commit, ok := snapshotGit(ctx, held, true, "rev-parse", "HEAD")
	if !ok || commit != request.Commit {
		return empty
	}
	tree, ok := snapshotGit(ctx, held, true, "rev-parse", "HEAD^{tree}")
	if !ok || tree != request.Tree {
		return empty
	}
	dirty, ok := snapshotGit(ctx, held, true, "status", "--porcelain", "--untracked-files=all")
	if !ok || ctx.Err() != nil {
		return empty
	}
	// Pathname replacement during measurement cannot transfer this observation.
	current, err := os.Lstat(request.ProjectRoot)
	original, heldErr := source.Stat()
	if err != nil || heldErr != nil || !os.SameFile(current, original) {
		return empty
	}
	return verificationSnapshotResponse{OK: true, Commit: commit, Tree: tree, Clean: dirty == ""}
}

func createVerificationSnapshot(ctx context.Context, request verificationSnapshotRequest) verificationSnapshotResponse {
	empty := verificationSnapshotResponse{}
	if !filepath.IsAbs(request.ProjectRoot) || len(request.ProjectRoot) > 4096 ||
		!snapshotHex(request.Commit, 40, 64) || !snapshotHex(request.Tree, 40, 64) || request.Lease != "" {
		return empty
	}
	verificationSnapshots.Lock()
	if len(verificationSnapshots.leases)+verificationSnapshots.creating >= verificationSnapshotsMax {
		verificationSnapshots.Unlock()
		return empty
	}
	verificationSnapshots.creating++
	verificationSnapshots.Unlock()
	defer func() {
		verificationSnapshots.Lock()
		verificationSnapshots.creating--
		verificationSnapshots.Unlock()
	}()
	source, err := os.OpenFile(request.ProjectRoot, os.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return empty
	}
	defer source.Close()
	if !snapshotRootIdentity(source, request.Device, request.Inode) {
		return empty
	}
	// Source metadata is in the same mounted storage namespace as its checkout.
	// An anonymous /tmp directory may be private to the server container and not
	// name the same resource to the Docker daemon. Git ignores this private child
	// under its actual metadata directory, including linked-worktree metadata.
	sourcePath := fmt.Sprintf("/proc/%d/fd/%d", os.Getpid(), source.Fd())
	metadata, ok := snapshotGit(ctx, sourcePath, true, "rev-parse", "--absolute-git-dir")
	if !ok || !filepath.IsAbs(metadata) {
		return empty
	}
	metadata, err = filepath.EvalSymlinks(metadata)
	if err != nil || len(metadata) > 4000 {
		return empty
	}
	parent, err := os.OpenFile(metadata, os.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return empty
	}
	defer parent.Close()
	heldParent := fmt.Sprintf("/proc/%d/fd/%d", os.Getpid(), parent.Fd())
	created, err := os.MkdirTemp(heldParent, "aimee-verification-")
	if err != nil {
		return empty
	}
	path := filepath.Join(metadata, filepath.Base(created))
	retained := false
	defer func() {
		if !retained {
			_ = os.RemoveAll(created)
		}
	}()
	// The public daemon path and held metadata capability must identify the same
	// new directory before any resource can be published for container execution.
	actual, actualErr := os.Lstat(path)
	heldInfo, heldErr := os.Stat(created)
	if actualErr != nil || heldErr != nil || !os.SameFile(actual, heldInfo) {
		return empty
	}
	// Reference the source's held directory, rather than reopening caller paths
	// after validation. The source stays held until fetch and all children join.
	if _, ok := snapshotGit(ctx, created, false, "init", "-q"); !ok {
		return empty
	}
	if _, ok := snapshotGit(ctx, created, false, "fetch", "-q", "--depth=1", "--no-tags", "--no-recurse-submodules", "--upload-pack=git-upload-pack", sourcePath, request.Commit); !ok {
		return empty
	}
	if _, ok := snapshotGit(ctx, created, false, "checkout", "-q", "--detach", "--no-recurse-submodules", request.Commit); !ok {
		return empty
	}
	tree, ok := snapshotGit(ctx, created, true, "rev-parse", "HEAD^{tree}")
	if !ok || tree != request.Tree {
		return empty
	}
	dirty, ok := snapshotGit(ctx, created, true, "status", "--porcelain", "--untracked-files=all")
	if !ok || dirty != "" {
		return empty
	}
	root, err := os.OpenFile(created, os.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return empty
	}
	var nonce [16]byte
	if _, err = rand.Read(nonce[:]); err != nil || ctx.Err() != nil {
		root.Close()
		return empty
	}
	lease := hex.EncodeToString(nonce[:])
	// The Engine-side worker reads this exact marker in its own namespace before
	// tests. A same-looking host path cannot satisfy an unrelated snapshot lease.
	marker, err := os.OpenFile(filepath.Join(created, ".git", "aimee-verification-lease"), os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0400)
	if err != nil {
		root.Close()
		return empty
	}
	_, writeErr := io.WriteString(marker, lease+"\n")
	modeErr := marker.Chmod(0400)
	syncErr := marker.Sync()
	closeErr := marker.Close()
	if writeErr != nil || modeErr != nil || syncErr != nil || closeErr != nil {
		root.Close()
		return empty
	}
	// Metadata path replacement during Git work cannot transfer publication to
	// a different directory. All construction above used the retained parent fd.
	published, publishErr := os.Lstat(path)
	original, rootErr := root.Stat()
	if publishErr != nil || rootErr != nil || !os.SameFile(published, original) {
		root.Close()
		return empty
	}
	verificationSnapshots.Lock()
	if _, exists := verificationSnapshots.leases[lease]; exists {
		verificationSnapshots.Unlock()
		root.Close()
		return empty
	}
	resource := &verificationSnapshot{root: root, path: path, tree: tree, commit: request.Commit, expires: time.Now().Add(verificationSnapshotTTL)}
	verificationSnapshots.leases[lease] = resource
	// A lost response cannot strand the bounded lease inventory indefinitely.
	// State checks renew custody; a verifier must recheck it before grading.
	resource.timer = time.AfterFunc(verificationSnapshotTTL, func() { expireVerificationSnapshot(lease, resource) })
	verificationSnapshots.Unlock()
	retained = true
	return verificationSnapshotResponse{OK: true, Lease: lease, Path: path, Tree: tree, Commit: request.Commit, Clean: true}
}

// Caller holds the lease lock. Refuse cleanup at a substituted pathname.
func releaseVerificationSnapshot(lease string, resource *verificationSnapshot) bool {
	delete(verificationSnapshots.leases, lease)
	resource.timer.Stop()
	info, err := os.Lstat(resource.path)
	original, heldErr := resource.root.Stat()
	removed := false
	if err == nil && heldErr == nil && os.SameFile(info, original) {
		removed = os.RemoveAll(resource.path) == nil
	}
	resource.root.Close()
	return removed
}

func expireVerificationSnapshot(lease string, expected *verificationSnapshot) {
	verificationSnapshots.Lock()
	defer verificationSnapshots.Unlock()
	resource := verificationSnapshots.leases[lease]
	if resource != expected {
		return
	}
	if remaining := time.Until(resource.expires); remaining > 0 {
		resource.timer.Reset(remaining)
		return
	}
	releaseVerificationSnapshot(lease, resource)
}

func useVerificationSnapshot(ctx context.Context, request verificationSnapshotRequest) verificationSnapshotResponse {
	empty := verificationSnapshotResponse{}
	if !snapshotHex(request.Lease, 32) || request.ProjectRoot != "" || request.Commit != "" || request.Tree != "" || request.Device != "" || request.Inode != "" {
		return empty
	}
	verificationSnapshots.Lock()
	defer verificationSnapshots.Unlock()
	resource := verificationSnapshots.leases[request.Lease]
	if resource == nil {
		return empty
	}
	if request.Op == "snapshot-release" {
		return verificationSnapshotResponse{OK: releaseVerificationSnapshot(request.Lease, resource)}
	}
	if request.Op != "snapshot-state" && request.Op != "snapshot-renew" {
		return empty
	}
	if time.Now().After(resource.expires) {
		releaseVerificationSnapshot(request.Lease, resource)
		return empty
	}
	held := fmt.Sprintf("/proc/%d/fd/%d", os.Getpid(), resource.root.Fd())
	info, err := os.Lstat(resource.path)
	original, heldErr := resource.root.Stat()
	if err != nil || heldErr != nil || !os.SameFile(info, original) {
		return empty
	}
	if ctx.Err() != nil {
		return empty
	}
	resource.expires = time.Now().Add(verificationSnapshotTTL)
	resource.timer.Reset(verificationSnapshotTTL)
	if request.Op == "snapshot-renew" {
		// Heartbeats keep custody alive while tests legitimately create build
		// files. They are not a cleanliness or immutable-source measurement.
		return verificationSnapshotResponse{OK: true, Lease: request.Lease, Path: resource.path}
	}
	commit, ok := snapshotGit(ctx, held, true, "rev-parse", "HEAD")
	if !ok || commit != resource.commit {
		return empty
	}
	tree, ok := snapshotGit(ctx, held, true, "rev-parse", "HEAD^{tree}")
	if !ok || tree != resource.tree {
		return empty
	}
	dirty, ok := snapshotGit(ctx, held, true, "status", "--porcelain", "--untracked-files=all")
	return verificationSnapshotResponse{OK: ok, Lease: request.Lease, Path: resource.path, Tree: tree, Commit: commit, Clean: ok && dirty == ""}
}

func handleVerificationSnapshot(invocation ring.ModuleInvocation, raw []byte) ([]byte, ring.ModuleStatus) {
	var request verificationSnapshotRequest
	if invocation.PrincipalRef != 0 || len(raw) > 16384 || !canonical.Decode(raw, &request) {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	if request.Op != "snapshot-source-state" && request.Op != "snapshot-create" && request.Op != "snapshot-state" && request.Op != "snapshot-renew" && request.Op != "snapshot-release" {
		return nil, ring.ModuleStatusInvalidRequest
	}
	ctx, cancel := context.WithTimeout(context.Background(), invocation.Remaining(time.Minute))
	defer cancel()
	stop, joined := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(joined)
		ticker := time.NewTicker(25 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ctx.Done():
				return
			case <-ticker.C:
				if invocation.Cancelled() {
					cancel()
					return
				}
			}
		}
	}()
	defer func() { close(stop); cancel(); <-joined }()
	var response verificationSnapshotResponse
	if request.Op == "snapshot-source-state" {
		response = measureVerificationSource(ctx, request)
	} else if request.Op == "snapshot-create" {
		response = createVerificationSnapshot(ctx, request)
	} else {
		response = useVerificationSnapshot(ctx, request)
	}
	if invocation.Cancelled() || ctx.Err() != nil {
		if request.Op == "snapshot-create" && response.Lease != "" {
			useVerificationSnapshot(context.Background(), verificationSnapshotRequest{Op: "snapshot-release", Lease: response.Lease})
		}
		return nil, ring.ModuleStatusCancelled
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return encoded, ring.ModuleStatusOK
}

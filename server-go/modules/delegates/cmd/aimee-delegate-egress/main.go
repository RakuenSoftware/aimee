// Command aimee-delegate-egress owns delegate container lifecycle and the
// sandbox module's sole network-capable proxy path. The legacy C daemon may
// provide filesystem facts and an accepted Unix fd, but cannot select a network
// mode, widen destinations, or override a failed posture verdict.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"encoding/json"
	"github.com/JBailes/aimee/server-go/modules/delegates"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/sandbox"
)

func commandRunner(ctx context.Context, name string, args ...string) (string, error) {
	output, err := exec.CommandContext(ctx, name, args...).Output()
	if failed, ok := err.(*exec.ExitError); ok && len(failed.Stderr) != 0 {
		return string(failed.Stderr), err
	}
	return string(output), err
}

// privateControlSocket accepts only the C owner's private nonce-scoped listener.
// The caller retains the socket lease; the helper checks its namespace mapping
// and rechecks these same filesystem identities after the runtime mount verdict.
func privateControlSocket(check, source, mounts string) ([]os.FileInfo, error) {
	parent := filepath.Dir(check)
	owners := filepath.Dir(parent)
	nonce := filepath.Base(parent)
	if !filepath.IsAbs(check) || filepath.Clean(check) != check || !filepath.IsAbs(source) || filepath.Clean(source) != source || filepath.Base(check) != delegates.ControlSocketBasename ||
		filepath.Base(owners) != "acp-owners" || len(nonce) != 64 || strings.Trim(nonce, "0123456789abcdef") != "" ||
		delegates.TranslateMountPath(check, mounts) != source {
		return nil, errors.New("private owner control socket identity required")
	}
	var result []os.FileInfo
	for i, path := range []string{check, parent, owners} {
		info, err := os.Lstat(path)
		if err != nil {
			return nil, err
		}
		stat, ok := info.Sys().(*syscall.Stat_t)
		mode := os.FileMode(0700)
		if i == 0 {
			mode = 0600
		}
		if !ok || int(stat.Uid) != os.Geteuid() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm() != mode ||
			(i == 0 && info.Mode()&os.ModeSocket == 0) || (i != 0 && !info.IsDir()) {
			return nil, errors.New("private owner control socket posture unavailable")
		}
		result = append(result, info)
	}
	return result, nil
}

// acquireFailure carries only a fixed public phase across the helper boundary.
// The underlying error remains available to in-process callers, never the C log.
type acquireFailure struct {
	stage string
	cause error
}

func (e *acquireFailure) Error() string { return e.cause.Error() }
func (e *acquireFailure) Unwrap() error { return e.cause }
func writeCommandFailure(w io.Writer, err error) {
	var failure *acquireFailure
	if errors.As(err, &failure) {
		_ = json.NewEncoder(w).Encode(struct {
			Schema string `json:"schema"`
			Stage  string `json:"stage"`
		}{"aimee.delegate-acquire-error.v1", failure.stage})
		return
	}
	fmt.Fprintf(w, "aimee-delegate-egress: %v\n", err)
}

func acquire(ctx context.Context, args []string, run delegates.CommandRunner) (resultText string, failure error) {
	stage, checked := "arguments", false
	defer func() {
		if checked && failure != nil {
			failure = &acquireFailure{stage, failure}
		}
	}()
	flags := flag.NewFlagSet("acquire", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	controllerBinds := flags.String("controller-binds", "", "producer-attested controller bind recursion facts")
	leaseFD := flags.Int("lease-fd", -1, "inherited private durable creation lease")
	leaseCheck := flags.String("lease-check", "", "private durable creation lease path")
	engineEndpoint := flags.String("engine-endpoint", "", "launching owner's held Docker Unix endpoint")
	helperCheck := flags.String("helper-check", "", "private measured owner helper path")
	helperSource := flags.String("helper-source", "", "daemon-visible measured owner helper path")
	helperSHA := flags.String("helper-sha256", "", "launching owner's helper measurement")
	resourceReceipt := flags.Bool("resource-receipt-json", false, "checked ordinary resource receipt; no private IO owner")
	receiptJSON := flags.Bool("receipt-json", false, "return the verified immutable acquisition receipt")
	docker := flags.String("docker", "docker", "container runtime binary")
	task := flags.String("task", "", "delegate task ID")
	image := flags.String("image", "", "sandbox image")
	workdir := flags.String("workdir", "", "container working directory")
	user := flags.String("user", "", "numeric uid:gid")
	repoRoot := flags.String("repo-root", "", "repository root")
	worktree := flags.String("worktree", "", "full source worktree")
	gitdir := flags.String("gitdir", "", "linked worktree git directory")
	writes := flags.Bool("writes-allowed", false, "delegate owns a writable worktree")
	socketSource := flags.String("socket-source", "", "daemon-visible aimee-http.sock path")
	socketCheck := flags.String("socket-check", "", "process-visible aimee-http.sock path")
	proxy := flags.Bool("proxy", false, "enable the mediated package proxy")
	mountTable := flags.String("mount-table", "", "daemon namespace mount translation table")
	if err := flags.Parse(args); err != nil {
		return "", err
	}
	checked = *receiptJSON || *resourceReceipt
	if flags.NArg() != 0 {
		return "", fmt.Errorf("unexpected positional arguments")
	}

	stage = "control-socket"
	info, err := os.Lstat(*socketCheck)
	if err != nil {
		return "", fmt.Errorf("required sole-egress socket: %w", err)
	}
	if info.Mode()&os.ModeSymlink != 0 || info.Mode()&os.ModeSocket == 0 {
		return "", fmt.Errorf("required sole-egress path is not a Unix socket")
	}
	if *worktree == "" || *workdir == "" {
		return "", fmt.Errorf("a full source worktree and workdir are required")
	}

	if *receiptJSON && *resourceReceipt {
		return "", errors.New("one receipt resource class required")
	}
	if *resourceReceipt && (*helperCheck != "" || *helperSource != "" || *helperSHA != "") {
		return "", errors.New("ordinary resource has no private helper")
	}
	checked = *receiptJSON || *resourceReceipt
	if !checked && (*controllerBinds != "" || *engineEndpoint != "" || *helperCheck != "" || *helperSource != "" || *helperSHA != "" || *leaseFD >= 0 || *leaseCheck != "") {
		return "", errors.New("private lease inputs require an immutable receipt")
	}
	var lease *ownerLease
	var helper *os.File
	helperDigest := ""
	var socketIdentity []os.FileInfo
	if checked {
		if !ownerEngineEndpoint(*engineEndpoint) {
			return "", errors.New("launching owner's held Docker endpoint required")
		}
		supplied := run
		run = func(ctx context.Context, binary string, args ...string) (string, error) {
			return supplied(ctx, binary, append([]string{"--host", *engineEndpoint}, args...)...)
		}
		if *receiptJSON {
			socketIdentity, err = privateControlSocket(*socketCheck, *socketSource, *mountTable)
			if err != nil {
				return "", err
			}
			stage = "helper-identity"
			helper, helperDigest, err = measuredOwnerHelper(*helperCheck, *helperSource, *helperSHA, *socketCheck, *mountTable)
			if err != nil {
				return "", err
			}
			defer helper.Close()
		} else {
			if delegates.TranslateMountPath(*socketCheck, *mountTable) != *socketSource {
				return "", errors.New("ordinary control socket namespace differs")
			}
			parentInfo, failure := os.Lstat(filepath.Dir(*leaseCheck))
			if failure != nil {
				return "", failure
			}
			socketIdentity = []os.FileInfo{info, parentInfo}
		}

		stage = "lease-open"
		if *receiptJSON {
			lease, err = openOwnerLease(*leaseFD, *leaseCheck, *socketCheck)
		} else {
			lease, err = openJournal(*leaseFD, *leaseCheck, false)
		}
		if err != nil {
			return "", err
		}
		defer lease.file.Close()
		lease.engine, lease.helperSHA = *engineEndpoint, helperDigest
		stage = "daemon-identity"
		lease.daemonID, err = ownerDaemonID(ctx, run, *docker)
		if err != nil {
			return "", err
		}
		if *receiptJSON {
			helperInfo, err := helper.Stat()
			if err != nil {
				return "", err
			}
			lease.artifacts = &ownerSourceIdentities{Socket: sourceIdentity(socketIdentity[0]), Parent: sourceIdentity(socketIdentity[1]), Helper: sourceIdentity(helperInfo)}
		} else {
			lease.schema = ordinaryLeaseSchema
			lease.resourceMode = ordinaryResourceMode
			lease.artifacts = &ownerSourceIdentities{Parent: sourceIdentity(socketIdentity[1])}
		}

	}

	request := delegates.SandboxRequest{
		WritesAllowed: *writes, RepoRoot: *repoRoot, Worktree: *worktree, GitDir: *gitdir,
		IsGitCheckout: true, ParentSocketHost: *socketSource,
		ParentSocketTarget: delegates.ControlSocketTarget, RunAsUser: *user,
	}
	if *proxy {
		request.EgressProxy = "http://127.0.0.1:3129"
	}
	stage = "sandbox-spec"
	var spec delegates.SandboxSpec
	if *receiptJSON {
		spec, err = delegates.BuildOwnerSandboxSpec(request, *helperSource)
	} else {
		spec, err = delegates.BuildSandboxSpec(request)
	}
	if err != nil {
		return "", err
	}
	if checked {
		stage = "controller-custody"
		if err := verifyControllerBinds(*controllerBinds, *mountTable, *leaseCheck, spec); err != nil {
			return "", err
		}
	}
	stage = "container-name"
	name, err := delegates.ContainerName(*task, spec, *mountTable)
	if err != nil {
		return "", err
	}
	dockerRequest := delegates.DockerCreateRequest{Spec: spec, ContainerName: name, Image: *image,
		WorkDir: *workdir, MountTable: *mountTable, Command: []string{"sleep", "infinity"}}
	runner := delegates.ContainerRunner{Docker: *docker, Run: run, MountTable: *mountTable, RequireImmutableIdentity: checked}
	if lease != nil {
		runner.Journal = lease
	}
	var result delegates.ContainerResult
	if *resourceReceipt {
		stage = "image-observation"
		dockerRequest.Image, dockerRequest.Startup, err = delegates.ResolveOrdinaryImage(ctx, run, *docker, *image)
		if err != nil {
			return "", err
		}
		dockerRequest.LeaseNonce = filepath.Base(filepath.Dir(*leaseCheck))
		stage = "container-start"
		result, err = runner.Start(ctx, dockerRequest)
	} else {
		stage = "container-start"
		result, err = runner.Acquire(ctx, dockerRequest)
	}
	if err != nil {
		return "", err
	}
	stage = "posture"
	if result.Refused {
		return "", errors.New(result.Reason)
	}
	stage = "receipt-source"
	if checked {
		var current []os.FileInfo
		var err error
		if *receiptJSON {
			current, err = privateControlSocket(*socketCheck, *socketSource, *mountTable)
		} else {
			currentSocket, failure := os.Lstat(*socketCheck)
			if failure != nil || !os.SameFile(currentSocket, info) {
				err = errors.New("global control socket changed during acquisition")
			}
		}

		if err == nil {
			for i := range current {
				if !os.SameFile(current[i], socketIdentity[i]) {
					err = errors.New("private control socket changed during acquisition")
					break
				}
			}
		}
		if err == nil && *receiptJSON {
			_, err = verifyOwnerHelper(helper, *helperCheck, helperDigest)
		}
		if err != nil {
			if cleanup := runner.Stop(ctx, result.ID); cleanup != nil {
				return "", fmt.Errorf("%w; cleanup: %v", err, cleanup)
			}
			return "", err
		}
		stage = "receipt-encoding"
		if *resourceReceipt {
			encoded, failure := json.Marshal(struct {
				Schema       string `json:"schema"`
				ResourceMode string `json:"resource_mode"`
				Name         string `json:"name"`
				ID           string `json:"container_id"`
				Image        string `json:"image_id"`
				WorkDir      string `json:"workdir"`
				Engine       string `json:"engine_endpoint"`
				SocketSource string `json:"control_socket_source"`
				SocketTarget string `json:"control_socket_target"`
				Verified     bool   `json:"verified_posture"`
				Fresh        bool   `json:"fresh_container"`
			}{"aimee.container-resource.v1", ordinaryResourceMode, result.Name, result.ID, result.ImageID, result.WorkDir, *engineEndpoint, *socketSource, delegates.ControlSocketTarget, true, true})
			return string(encoded), failure
		}
		receipt := struct {
			Schema              string `json:"schema"`
			Name                string `json:"name"`
			ContainerID         string `json:"container_id"`
			ImageID             string `json:"image_id"`
			WorkDir             string `json:"workdir"`
			VerifiedPosture     bool   `json:"verified_posture"`
			ControlSocketSource string `json:"control_socket_source"`
			ControlSocketTarget string `json:"control_socket_target"`
			EngineEndpoint      string `json:"engine_endpoint"`
			HelperSource        string `json:"helper_source"`
			HelperTarget        string `json:"helper_target"`
			HelperSHA256        string `json:"helper_sha256"`
			TrustedStartup      string `json:"trusted_startup"`
			FreshContainer      bool   `json:"fresh_container"`
		}{
			Schema: "aimee.container-acquire.v1", Name: result.Name, ContainerID: result.ID, ImageID: result.ImageID,
			WorkDir: result.WorkDir, VerifiedPosture: true, ControlSocketSource: *socketSource,
			ControlSocketTarget: delegates.ControlSocketTarget, EngineEndpoint: *engineEndpoint,
			HelperSource: *helperSource, HelperTarget: delegates.OwnerHelperTarget, HelperSHA256: helperDigest,
			TrustedStartup: "owner-io-idle-guard-v1", FreshContainer: true,
		}
		encoded, err := json.Marshal(receipt)
		return string(encoded), err
	}
	return result.Name, nil
}

func proxy(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("proxy", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	fd := flags.Int("fd", 3, "inherited Unix client socket descriptor")
	tag := flags.String("tag", "sandbox", "bounded audit label")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || *fd < 3 {
		return fmt.Errorf("invalid proxy arguments")
	}
	head, err := sandbox.ReadProxyHead(os.Stdin)
	if err != nil {
		return err
	}
	file := os.NewFile(uintptr(*fd), "delegate-egress-client")
	if file == nil {
		return fmt.Errorf("invalid inherited socket fd")
	}
	conn, err := net.FileConn(file)
	_ = file.Close()
	if err != nil {
		return fmt.Errorf("inherit client socket: %w", err)
	}
	defer conn.Close()
	if _, ok := conn.(*net.UnixConn); !ok {
		return fmt.Errorf("refusing egress for a non-Unix client socket")
	}
	destination, err := (sandbox.Proxy{}).Serve(ctx, conn, head)
	if err != nil {
		return fmt.Errorf("%s denied: %w", *tag, err)
	}
	fmt.Fprintf(os.Stderr, "aimee-delegate-egress: %s allowed host=%s port=%d resolved_ip=%s\n",
		*tag, destination.Host, destination.Port, destination.IP)
	return nil
}

// Verification custody ends with the server process that launched this helper.
// An expected PID is required: a helper first scheduled after its server dies
// must not adopt init (or a subreaper) as a new execution owner.
func verifierParentContext(ctx context.Context, expected int) (context.Context, func(), error) {
	if expected <= 0 || os.Getppid() != expected {
		return nil, nil, errors.New("verification parent identity unavailable")
	}
	owned, cancel := context.WithCancel(ctx)
	joined := make(chan struct{})
	go func() {
		defer close(joined)
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-owned.Done():
				return
			case <-ticker.C:
				if os.Getppid() != expected {
					cancel()
					return
				}
			}
		}
	}()
	return owned, func() { cancel(); <-joined }, nil
}

func verify(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("verify", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	socket := flags.String("engine-socket", "", "configured Docker Unix socket")
	parent := flags.Int("parent-pid", 0, "required launching server process ID")
	ownerState := flags.String("owner-state-dir", "", "required private resource owner state directory")
	image := flags.String("image", "", "configured private test image")
	directory := flags.String("workdir", "", "self-contained private Git snapshot")
	user := flags.String("user", "", "numeric uid:gid")
	mounts := flags.String("mount-table", "", "host mount translation facts")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("unexpected verifier positional arguments")
	}
	owned, joined, err := verifierParentContext(ctx, *parent)
	if err != nil {
		return err
	}
	defer joined()
	ctx = owned
	request, err := delegates.DecodePrivateTestRequest(os.Stdin)
	if err != nil {
		return err
	}
	openCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	engine, err := egress.OpenDockerEngine(openCtx, *socket)
	cancel()
	if err != nil {
		return err
	}
	defer engine.Close()
	lease, err := delegates.AcquirePrivateWorkerLease(*ownerState)
	if err != nil {
		return err
	}
	removed := false
	defer func() { lease.Close(removed) }()
	receipt := delegates.ExecutePrivateTests(ctx, request, delegates.PrivateTestFacts{Image: *image, WorkDir: *directory, User: *user, MountTable: *mounts, WorkerLabels: lease.Labels()}, engine)
	removed = receipt.RemovalAcknowledged
	// Execution failure is a receipt, not a transport exit-code verdict.
	return json.NewEncoder(os.Stdout).Encode(receipt)
}

func reapVerifiers(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("reap-verifiers", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	directory := flags.String("owner-state-dir", "", "private resource owner state directory")
	socket := flags.String("engine-socket", "", "configured Docker Unix socket")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("unexpected reaper positional arguments")
	}
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	engine, err := egress.OpenDockerEngine(ctx, *socket)
	if err != nil {
		return err
	}
	defer engine.Close()
	count, err := delegates.ReapPrivateWorkers(ctx, *directory, engine)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintln(os.Stdout, count)
	return err
}

func run(ctx context.Context, args []string) error {
	if len(args) >= 2 && (args[1] == "version" || args[1] == "--version") {
		fmt.Println("aimee-delegate-egress go-v1")
		return nil
	}
	if len(args) >= 2 && args[1] == "cleanup-lease" {
		return cleanupOwnerLease(ctx, args[2:], commandRunner)
	}
	if len(args) >= 2 && args[1] == "reap-verifiers" {
		return reapVerifiers(ctx, args[2:])
	}
	if len(args) >= 2 && args[1] == "verify" {
		return verify(ctx, args[2:])
	}
	if len(args) >= 2 && args[1] == "proxy" {
		return proxy(ctx, args[2:])
	}
	if len(args) < 2 || args[1] != "acquire" {
		return fmt.Errorf("usage: aimee-delegate-egress acquire [flags] | cleanup-lease [flags] | verify [flags] | reap-verifiers [flags] | proxy [flags] | version")
	}
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	name, err := acquire(ctx, args[2:], commandRunner)
	if err != nil {
		return err
	}
	fmt.Println(strings.TrimSpace(name))
	return nil
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args); err != nil {
		writeCommandFailure(os.Stderr, err)
		os.Exit(1)
	}
}

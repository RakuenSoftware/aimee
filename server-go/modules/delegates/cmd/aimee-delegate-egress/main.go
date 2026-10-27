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
	"net"
	"os"
	"os/exec"
	"os/signal"
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

func acquire(ctx context.Context, args []string, run delegates.CommandRunner) (string, error) {
	flags := flag.NewFlagSet("acquire", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
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
	if flags.NArg() != 0 {
		return "", fmt.Errorf("unexpected positional arguments")
	}

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

	request := delegates.SandboxRequest{
		WritesAllowed: *writes, RepoRoot: *repoRoot, Worktree: *worktree, GitDir: *gitdir,
		IsGitCheckout: true, ParentSocketHost: *socketSource,
		ParentSocketTarget: delegates.ControlSocketTarget, RunAsUser: *user,
	}
	if *proxy {
		request.EgressProxy = "http://127.0.0.1:3129"
	}
	spec, err := delegates.BuildSandboxSpec(request)
	if err != nil {
		return "", err
	}
	name, err := delegates.ContainerName(*task, spec, *mountTable)
	if err != nil {
		return "", err
	}
	dockerRequest := delegates.DockerCreateRequest{Spec: spec, ContainerName: name, Image: *image,
		WorkDir: *workdir, MountTable: *mountTable, Command: []string{"sleep", "infinity"}}
	result, err := (delegates.ContainerRunner{Docker: *docker, Run: run, MountTable: *mountTable}).Acquire(ctx, dockerRequest)
	if err != nil {
		return "", err
	}
	if result.Refused {
		return "", errors.New(result.Reason)
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
		return fmt.Errorf("usage: aimee-delegate-egress acquire [flags] | verify [flags] | reap-verifiers [flags] | proxy [flags] | version")
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
		fmt.Fprintf(os.Stderr, "aimee-delegate-egress: %v\n", err)
		os.Exit(1)
	}
}

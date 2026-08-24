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

	"github.com/JBailes/aimee/server-go/modules/delegates"
	"github.com/JBailes/aimee/server-go/modules/sandbox"
)

func commandRunner(ctx context.Context, name string, args ...string) (string, error) {
	output, err := exec.CommandContext(ctx, name, args...).CombinedOutput()
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

func run(ctx context.Context, args []string) error {
	if len(args) >= 2 && (args[1] == "version" || args[1] == "--version") {
		fmt.Println("aimee-delegate-egress go-v1")
		return nil
	}
	if len(args) >= 2 && args[1] == "proxy" {
		return proxy(ctx, args[2:])
	}
	if len(args) < 2 || args[1] != "acquire" {
		return fmt.Errorf("usage: aimee-delegate-egress acquire [flags] | proxy [flags] | version")
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

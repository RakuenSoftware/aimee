//go:build !windows

package supervisor

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"
)

func child(ctx context.Context, executable, socket, role, home string) error {
	cmd := exec.Command(executable, socket)
	for _, value := range os.Environ() {
		if !strings.HasPrefix(value, "AIMEE_MODULE_PLACEMENT=") && !strings.HasPrefix(value, "AIMEE_HOME=") {
			cmd.Env = append(cmd.Env, value)
		}
	}
	cmd.Env = append(cmd.Env, "AIMEE_MODULE_PLACEMENT="+role, "AIMEE_HOME="+home)
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		// Any orphan descendants are part of this failed module, too.
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		return err
	case <-ctx.Done():
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM)
	}
	timer := time.NewTimer(5 * time.Second)
	defer timer.Stop()
	select {
	case err := <-done:
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		return err
	case <-timer.C:
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		return errors.Join(ctx.Err(), <-done)
	}
}

//go:build linux

package main

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"golang.org/x/sys/unix"
)

func TestModuleNetworkGuardDeniesInternetAndRetainsTheBus(t *testing.T) {
	if os.Getenv("AIMEE_TEST_NETWORK_GUARD_HELPER") == "1" {
		if err := installModuleNetworkGuard(); err != nil {
			t.Fatal(err)
		}
		if listener, err := net.Listen("tcp4", "127.0.0.1:0"); err == nil {
			_ = listener.Close()
			t.Fatal("IPv4 socket creation survived the module network guard")
		}
		path := filepath.Join(t.TempDir(), "bus.sock")
		listener, err := net.Listen("unix", path)
		if err != nil {
			t.Fatalf("AF_UNIX must remain available for the module bus: %v", err)
		}
		_ = listener.Close()
		return
	}
	command := exec.Command(os.Args[0], "-test.run=^TestModuleNetworkGuardDeniesInternetAndRetainsTheBus$")
	command.Env = append(os.Environ(), "AIMEE_TEST_NETWORK_GUARD_HELPER=1")
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("network-guard subprocess: %v\n%s", err, strings.TrimSpace(string(output)))
	}
}

// Exercise TSYNC on threads that existed before installation.
func TestModuleNetworkGuardSynchronizesExistingThreads(t *testing.T) {
	if os.Getenv("AIMEE_TEST_NETWORK_GUARD_THREADS") == "1" {
		runtime.GOMAXPROCS(4)
		const workers = 8
		ready := make(chan struct{}, workers)
		start := make(chan struct{})
		results := make(chan error, workers)
		defer close(start)
		for range workers {
			go func() {
				runtime.LockOSThread()
				defer runtime.UnlockOSThread()
				ready <- struct{}{}
				<-start
				for _, family := range []int{unix.AF_INET, unix.AF_INET6} {
					fd, err := unix.Socket(family, unix.SOCK_STREAM, 0)
					if err == nil {
						_ = unix.Close(fd)
					}
					if err != unix.EPERM {
						results <- fmt.Errorf("family %d on existing thread: got %v, want EPERM", family, err)
						return
					}
				}
				fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_STREAM, 0)
				if err == nil {
					_ = unix.Close(fd)
				}
				results <- err
			}()
		}
		for range workers {
			<-ready
		}
		if err := installModuleNetworkGuard(); err != nil {
			t.Fatal(err)
		}
		for range workers {
			start <- struct{}{}
		}
		for range workers {
			if err := <-results; err != nil {
				t.Fatal(err)
			}
		}
		return
	}
	// Use fresh processes: a successful TSYNC propagates no_new_privs and would
	// otherwise conceal a startup race in later attempts on the same process.
	for i := range 12 {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			command := exec.Command(os.Args[0], "-test.run=^TestModuleNetworkGuardSynchronizesExistingThreads$")
			command.Env = append(os.Environ(), "AIMEE_TEST_NETWORK_GUARD_THREADS=1", "GOMAXPROCS=4")
			output, err := command.CombinedOutput()
			if err != nil {
				t.Fatalf("network-guard subprocess: %v\n%s", err, strings.TrimSpace(string(output)))
			}
		})
	}
}

func TestEgressCredentialOwnerIsNonDumpable(t *testing.T) {
	if os.Getenv("AIMEE_TEST_EGRESS_DUMP_HELPER") == "1" {
		if err := hardenEgressCredentialOwner(); err != nil {
			t.Fatal(err)
		}
		value, _, errno := unix.Syscall(unix.SYS_PRCTL, unix.PR_GET_DUMPABLE, 0, 0)
		if errno != 0 || value != 0 {
			t.Fatalf("PR_GET_DUMPABLE = %d, errno=%v", value, errno)
		}
		return
	}
	command := exec.Command(os.Args[0], "-test.run=^TestEgressCredentialOwnerIsNonDumpable$")
	command.Env = append(os.Environ(), "AIMEE_TEST_EGRESS_DUMP_HELPER=1")
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("egress-hardening subprocess: %v\n%s", err, strings.TrimSpace(string(output)))
	}
}
